import fs from "node:fs/promises";
import path from "node:path";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";


function parseArgs(argv) {
  const args = {};
  for (let i = 2; i < argv.length; i += 2) {
    args[argv[i].replace(/^--/, "")] = argv[i + 1];
  }
  for (const required of ["root", "campaign", "output-dir"]) {
    if (!args[required]) throw new Error(`--${required} est requis`);
  }
  return args;
}


function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = "";
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (quoted) {
      if (char === '"' && text[i + 1] === '"') {
        field += '"';
        i++;
      } else if (char === '"') {
        quoted = false;
      } else {
        field += char;
      }
    } else if (char === '"') {
      quoted = true;
    } else if (char === ",") {
      row.push(field);
      field = "";
    } else if (char === "\n") {
      row.push(field.replace(/\r$/, ""));
      rows.push(row);
      row = [];
      field = "";
    } else {
      field += char;
    }
  }
  if (field !== "" || row.length) {
    row.push(field.replace(/\r$/, ""));
    rows.push(row);
  }
  const headers = rows.shift() || [];
  return rows
    .filter((values) => values.some((value) => value !== ""))
    .map((values) => Object.fromEntries(
      headers.map((header, index) => [header, values[index] ?? ""]),
    ));
}


async function loadCsv(filePath) {
  try {
    return parseCsv(await fs.readFile(filePath, "utf8"));
  } catch (error) {
    if (error.code === "ENOENT") return [];
    throw error;
  }
}


function typed(value, header) {
  if (value === null || value === undefined || value === "") return null;
  const text = String(value);
  const textFields = new Set([
    "run_id", "date", "data_origin", "model", "variant_name",
    "raw_log_path", "raw_log_sha256", "parse_status", "result_class",
    "parser_warnings", "client_end_reason", "server_end_reason",
    "issue", "severity",
  ]);
  if (textFields.has(header) || /sha256|path|name|reason|status|origin/.test(header)) {
    return text;
  }
  if (/^(true|false)$/i.test(text)) return text.toLowerCase() === "true";
  if (/^-?\d+$/.test(text)) return Number(text);
  if (/^-?(?:\d+\.\d*|\d*\.\d+)(?:e[-+]?\d+)?$/i.test(text)) {
    return Number(text);
  }
  return text;
}


function colLetter(index) {
  let value = index + 1;
  let output = "";
  while (value > 0) {
    value--;
    output = String.fromCharCode(65 + (value % 26)) + output;
    value = Math.floor(value / 26);
  }
  return output;
}


function uniqueTableName(base, kem) {
  return `${base}${kem}`.replace(/[^A-Za-z0-9_]/g, "");
}


function styleTableSheet(sheet, headers, rows, tableName) {
  const matrix = [
    headers,
    ...rows.map((row) => headers.map((header) => typed(row[header], header))),
  ];
  const lastCol = colLetter(headers.length - 1);
  const lastRow = Math.max(2, matrix.length);
  if (matrix.length === 1) {
    matrix.push(headers.map(() => null));
  }
  sheet.getRange(`A1:${lastCol}${matrix.length}`).values = matrix;
  sheet.getRange(`A1:${lastCol}1`).format = {
    fill: "#17324D",
    font: { bold: true, color: "#FFFFFF" },
    wrapText: true,
    verticalAlignment: "center",
    borders: { preset: "outside", style: "thin", color: "#0F2438" },
  };
  sheet.getRange(`A1:${lastCol}1`).format.rowHeight = 34;
  sheet.freezePanes.freezeRows(1);
  sheet.showGridLines = false;
  const table = sheet.tables.add(
    `A1:${lastCol}${Math.max(2, matrix.length)}`,
    true,
    tableName,
  );
  table.style = "TableStyleMedium2";
  table.showFilterButton = true;
  headers.forEach((header, index) => {
    const letter = colLetter(index);
    const sample = rows.slice(0, 50).map((row) => String(row[header] ?? ""));
    const longest = Math.max(header.length, ...sample.map((value) => value.length));
    let width = Math.min(28, Math.max(10, longest + 2));
    if (/sha256/.test(header)) width = 22;
    if (/warnings|raw_log_path/.test(header)) width = 28;
    if (header === "date") width = 20;
    sheet.getRange(`${letter}1:${letter}${lastRow}`).format.columnWidth = width;
    if (header === "date") {
      sheet.getRange(`${letter}2:${letter}${lastRow}`).format.numberFormat = "yyyy-mm-dd hh:mm:ss";
    } else if (/prr|rate/.test(header)) {
      sheet.getRange(`${letter}2:${letter}${lastRow}`).format.numberFormat = "0.00%";
    } else if (/energy.*uJ|rtt_ms|elapsed_ms|one_way_ms/.test(header)) {
      sheet.getRange(`${letter}2:${letter}${lastRow}`).format.numberFormat = "#,##0.0";
    } else if (/size|bytes|ticks|attempt|retries|timeouts|frags|seed|variant|kem_level|count/.test(header)) {
      sheet.getRange(`${letter}2:${letter}${lastRow}`).format.numberFormat = "#,##0";
    }
  });
  return { lastCol, lastRow: matrix.length };
}


function formulaCriteria(helperColumns, rowNumber, maxRow, includeBurst) {
  const ref = (header) => {
    const letter = colLetter(helperColumns.get(header));
    return `$${letter}$2:$${letter}$${maxRow}`;
  };
  const criteria = [
    ref("model"), `A${rowNumber}`,
    ref("target_prr"), `B${rowNumber}`,
    ref("variant"), `D${rowNumber}`,
  ];
  if (includeBurst) {
    criteria.splice(4, 0, ref("mean_bad_burst"), `C${rowNumber}`);
  }
  return criteria;
}


function summarySheet(workbook, runs, headers, kem) {
  const sheet = workbook.worksheets.add("Summary");
  sheet.showGridLines = false;
  const groupMap = new Map();
  for (const run of runs) {
    const key = [
      run.model, run.target_prr, run.mean_bad_burst,
      run.variant, run.variant_name,
    ].join("|");
    groupMap.set(key, {
      model: run.model,
      target_prr: run.target_prr,
      mean_bad_burst: run.mean_bad_burst,
      variant: run.variant,
      variant_name: run.variant_name,
    });
  }
  const groups = [...groupMap.values()].sort((a, b) =>
    a.model.localeCompare(b.model) ||
    Number(b.target_prr) - Number(a.target_prr) ||
    Number(a.mean_bad_burst || 0) - Number(b.mean_bad_burst || 0) ||
    Number(a.variant) - Number(b.variant)
  );
  const summaryHeaders = [
    "model", "target_prr", "mean_bad_burst", "variant", "variant_name",
    "number_of_runs", "successful_runs", "success_rate",
    "mean_rtt_ms_success", "mean_app_retries",
    "mean_client_radio_energy_uJ", "mean_server_radio_energy_uJ",
  ];
  const lastRow = groups.length + 1;
  sheet.getRange(`A1:L${lastRow}`).values = [
    summaryHeaders,
    ...groups.map((group) => [
      group.model,
      typed(group.target_prr, "target_prr"),
      typed(group.mean_bad_burst, "mean_bad_burst"),
      typed(group.variant, "variant"),
      group.variant_name,
      null, null, null, null, null, null, null,
    ]),
  ];
  /*
   * Bloc d'aide visible à droite : copie contrôlée des seules colonnes
   * nécessaires. Les formules restent ainsi auditables et calculables même
   * dans les moteurs qui ne résolvent pas les références inter-feuilles.
   */
  const helperHeaders = [
    "model", "target_prr", "mean_bad_burst", "variant", "client_success",
    "client_rtt_ms", "client_app_retries",
    "client_energy_tx_energy_uJ", "client_energy_rx_energy_uJ",
    "server_energy_tx_energy_uJ", "server_energy_rx_energy_uJ",
  ];
  const helperStart = 13;
  const helperColumns = new Map(
    helperHeaders.map((header, index) => [header, helperStart + index]),
  );
  const helperLastCol = colLetter(helperStart + helperHeaders.length - 1);
  sheet.getRange(`N1:${helperLastCol}${runs.length + 1}`).values = [
    helperHeaders,
    ...runs.map((run) => helperHeaders.map((header) => typed(run[header], header))),
  ];
  sheet.getRange(`N1:${helperLastCol}1`).format = {
    fill: "#D8E2EA",
    font: { bold: true, color: "#17324D" },
    wrapText: true,
  };
  for (let index = helperStart; index < helperStart + helperHeaders.length; index++) {
    const letter = colLetter(index);
    sheet.getRange(`${letter}1:${letter}${runs.length + 1}`).format.columnWidth = 18;
  }
  const maxRunsRow = Math.max(2, runs.length + 1);
  const runRef = (header) => {
    const letter = colLetter(helperColumns.get(header));
    return `$${letter}$2:$${letter}$${maxRunsRow}`;
  };
  for (let row = 2; row <= lastRow; row++) {
    const criteria = formulaCriteria(
      helperColumns, row, maxRunsRow, groups[row - 2].model === "gilbert",
    );
    const criteriaText = criteria.join(",");
    sheet.getRange(`F${row}:L${row}`).formulas = [[
      `=COUNTIFS(${criteriaText})`,
      `=COUNTIFS(${criteriaText},${runRef("client_success")},1)`,
      `=IFERROR(G${row}/F${row},0)`,
      `=IFERROR(AVERAGEIFS(${runRef("client_rtt_ms")},${criteriaText},${runRef("client_success")},1),"")`,
      `=IFERROR(AVERAGEIFS(${runRef("client_app_retries")},${criteriaText}),"")`,
      `=IFERROR(AVERAGEIFS(${runRef("client_energy_tx_energy_uJ")},${criteriaText}),0)+IFERROR(AVERAGEIFS(${runRef("client_energy_rx_energy_uJ")},${criteriaText}),0)`,
      `=IFERROR(AVERAGEIFS(${runRef("server_energy_tx_energy_uJ")},${criteriaText}),0)+IFERROR(AVERAGEIFS(${runRef("server_energy_rx_energy_uJ")},${criteriaText}),0)`,
    ]];
  }
  sheet.getRange("A1:L1").format = {
    fill: "#17324D",
    font: { bold: true, color: "#FFFFFF" },
    wrapText: true,
    borders: { preset: "outside", style: "thin", color: "#0F2438" },
  };
  sheet.getRange("A1:L1").format.rowHeight = 34;
  sheet.getRange(`B2:B${lastRow}`).format.numberFormat = "0.00%";
  sheet.getRange(`H2:H${lastRow}`).format.numberFormat = "0.0%";
  sheet.getRange(`I2:L${lastRow}`).format.numberFormat = "#,##0.0";
  ["A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L"].forEach((letter, index) => {
    sheet.getRange(`${letter}1:${letter}${lastRow}`).format.columnWidth =
      [12, 14, 16, 10, 35, 16, 16, 14, 20, 18, 25, 25][index];
  });
  sheet.freezePanes.freezeRows(1);
  const table = sheet.tables.add(
    `A1:L${Math.max(2, lastRow)}`, true, uniqueTableName("Summary", kem),
  );
  table.style = "TableStyleMedium2";
  sheet.getRange(`H2:H${lastRow}`).conditionalFormats.add("colorScale", {
    colors: ["#FECACA", "#FEF3C7", "#BBF7D0"],
    thresholds: ["min", "50%", "max"],
  });
  return sheet;
}


function readmeSheet(workbook, runs, headers, kem) {
  const sheet = workbook.worksheets.add("README");
  sheet.showGridLines = false;
  sheet.getRange("A1:F2").merge();
  sheet.getRange("A1").values = [[`Résultats ML-KEM-${kem}`]];
  sheet.getRange("A1:F2").format = {
    fill: "#17324D",
    font: { bold: true, color: "#FFFFFF", size: 20 },
    verticalAlignment: "center",
    horizontalAlignment: "left",
  };
  const origins = [...new Set(runs.map((run) => run.data_origin).filter(Boolean))];
  const synthetic = origins.some((origin) => origin.includes("SYNTHETIC"));
  sheet.getRange("A4:F4").merge();
  sheet.getRange("A4").values = [[
    synthetic
      ? "VALIDATION DU PARSEUR — DONNÉES SYNTHÉTIQUES, PAS DES MESURES SCIENTIFIQUES"
      : "DONNÉES DE SIMULATION COOJA — VÉRIFIER LA FEUILLE QC AVANT ANALYSE",
  ]];
  sheet.getRange("A4:F4").format = {
    fill: synthetic ? "#FDE68A" : "#D1FAE5",
    font: { bold: true, color: "#7C2D12" },
    wrapText: true,
  };
  const definitions = [
    ["Indicateur", "Valeur / définition"],
    ["Niveau", `ML-KEM-${kem}`],
    ["Nombre de runs", null],
    ["Succès", null],
    ["Taux de succès", null],
    ["Origine", origins.join(" | ") || "non renseignée"],
    ["Source de vérité", "runs/**/cooja.log, identifié par chemin + SHA-256"],
    ["Succès global", "B1–V4: succès client + serveur; V5: récupération client après échec initial autorisée"],
    ["Latence", "RTT client; moyenne calculée uniquement sur les succès"],
    ["Énergie", "ticks Energest primaires; µJ = estimation du modèle configuré"],
    ["Gilbert", "p_bg=1/E[L] et p_gb=p_bg*(1-PRR)/PRR"],
    ["V5", "SUCCESS peut reposer sur client_success=1, app_retries>0 et un résultat serveur initial présent"],
  ];
  sheet.getRange("A6:B17").values = definitions;
  const helperMaxRow = 19 + runs.length;
  sheet.getRange(`H19:H${helperMaxRow}`).values = [
    ["result_class"],
    ...runs.map((run) => [run.result_class]),
  ];
  sheet.getRange("H19").format = {
    fill: "#D8E2EA",
    font: { bold: true, color: "#17324D" },
  };
  sheet.getRange("H19:H19").format.columnWidth = 18;
  sheet.getRange("B8").formulas = [[`=COUNTA(H20:H${helperMaxRow})`]];
  sheet.getRange("B9").formulas = [[`=COUNTIF(H20:H${helperMaxRow},"SUCCESS")`]];
  sheet.getRange("B10").formulas = [["=IFERROR(B9/B8,0)"]];
  sheet.getRange("B10").format.numberFormat = "0.0%";
  sheet.getRange("A6:B6").format = {
    fill: "#2A6F97",
    font: { bold: true, color: "#FFFFFF" },
  };
  sheet.getRange("A6:A17").format.font = { bold: true, color: "#17324D" };
  sheet.getRange("A6:B17").format.borders = {
    preset: "inside", style: "thin", color: "#D8E2EA",
  };
  sheet.getRange("A1:A17").format.columnWidth = 24;
  sheet.getRange("B1:B17").format.columnWidth = 78;
  sheet.getRange("B6:B17").format.wrapText = true;
  return sheet;
}


async function buildOne(kem, allRuns, campaignRows, issues, root, outputDir) {
  const runs = allRuns.filter((row) => Number(row.kem_level) === kem);
  const campaign = campaignRows.filter((row) => Number(row.kem_level) === kem);
  const runIds = new Set(runs.map((row) => row.run_id));
  const qcRows = issues.filter((issue) => !issue.run_id || runIds.has(issue.run_id));
  if (!runs.length) throw new Error(`Aucun run pour ML-KEM-${kem}`);
  const headers = Object.keys(allRuns[0]);
  const workbook = Workbook.create();
  readmeSheet(workbook, runs, headers, kem);

  const runsSheet = workbook.worksheets.add("Runs");
  const runsLayout = styleTableSheet(
    runsSheet, headers, runs, uniqueTableName("Runs", kem),
  );
  const successIndex = headers.indexOf("client_success");
  if (successIndex >= 0) {
    const letter = colLetter(successIndex);
    runsSheet.getRange(`${letter}2:${letter}${runsLayout.lastRow}`)
      .conditionalFormats.add("cellIs", {
        operator: "equal",
        formula: 1,
        format: { fill: "#DCFCE7", font: { color: "#166534", bold: true } },
      });
    runsSheet.getRange(`${letter}2:${letter}${runsLayout.lastRow}`)
      .conditionalFormats.add("cellIs", {
        operator: "equal",
        formula: 0,
        format: { fill: "#FEE2E2", font: { color: "#991B1B", bold: true } },
      });
  }
  summarySheet(workbook, runs, headers, kem);

  const campaignSheet = workbook.worksheets.add("Campaign");
  const campaignHeaders = Object.keys(campaign[0] || { information: "" });
  styleTableSheet(
    campaignSheet,
    campaignHeaders,
    campaign,
    uniqueTableName("Campaign", kem),
  );

  const evidenceHeaders = [
    "run_id", "data_origin", "model", "target_prr", "mean_bad_burst",
    "variant", "raw_log_path", "raw_log_sha256", "log_size_bytes",
    "log_line_count", "parse_status", "result_class",
  ];
  const evidenceSheet = workbook.worksheets.add("Log Evidence");
  styleTableSheet(
    evidenceSheet,
    evidenceHeaders,
    runs,
    uniqueTableName("Evidence", kem),
  );

  const qcSheet = workbook.worksheets.add("QC");
  const qcData = qcRows.length
    ? qcRows
    : [{ run_id: "", severity: "OK", issue: "Aucune anomalie détectée" }];
  styleTableSheet(
    qcSheet,
    ["run_id", "severity", "issue"],
    qcData,
    uniqueTableName("QC", kem),
  );

  const keyInspect = await workbook.inspect({
    kind: "table",
    range: "Summary!A1:L20",
    include: "values,formulas",
    tableMaxRows: 20,
    tableMaxCols: 12,
    maxChars: 6000,
  });
  const errors = await workbook.inspect({
    kind: "match",
    searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
    options: { useRegex: true, maxResults: 200 },
    summary: `formula scan ML-KEM-${kem}`,
  });
  console.log(keyInspect.ndjson);
  console.log(errors.ndjson);

  const previewDir = path.join(outputDir, "previews", `mlkem${kem}`);
  await fs.mkdir(previewDir, { recursive: true });
  const previewRanges = {
    "README": "A1:F17",
    "Runs": "A1:M12",
    "Summary": "A1:L20",
    "Campaign": "A1:L12",
    "Log Evidence": "A1:L12",
    "QC": "A1:C12",
  };
  for (const [sheetName, range] of Object.entries(previewRanges)) {
    const image = await workbook.render({
      sheetName,
      range,
      scale: 1,
      format: "png",
    });
    await fs.writeFile(
      path.join(previewDir, `${sheetName.replace(/ /g, "_")}.png`),
      new Uint8Array(await image.arrayBuffer()),
    );
  }
  await fs.mkdir(outputDir, { recursive: true });
  const output = await SpreadsheetFile.exportXlsx(workbook);
  const outputPath = path.join(outputDir, `MLKEM${kem}_results.xlsx`);
  await output.save(outputPath);
  return outputPath;
}


const args = parseArgs(process.argv);
const root = path.resolve(args.root);
const outputDir = path.resolve(args["output-dir"]);
const allRuns = await loadCsv(path.join(root, "results", "all_runs.csv"));
const campaign = await loadCsv(path.resolve(args.campaign));
const issues = await loadCsv(path.join(root, "results", "validation_issues.csv"));
const outputs = [];
for (const kem of [512, 768, 1024]) {
  outputs.push(await buildOne(kem, allRuns, campaign, issues, root, outputDir));
}
console.log(JSON.stringify({ outputs }));
