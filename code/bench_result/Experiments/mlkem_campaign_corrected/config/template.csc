<?xml version="1.0" encoding="UTF-8"?>
<simconf version="2023090101">
  <project EXPORT="discard">[CONTIKI_DIR]/tools/cooja/apps/mspsim</project>
  <simulation>
    <title>[RUN_ID]</title>
    <speedlimit>null</speedlimit>
    <randomseed>[SEED]</randomseed>
    <motedelay_us>0</motedelay_us>
    <radiomedium>
      [RADIO_MEDIUM_CLASS]
      <transmitting_range>50.0</transmitting_range>
      <interference_range>100.0</interference_range>
      <success_ratio_tx>1.0</success_ratio_tx>
      [RADIO_MODEL_PARAMETERS]
    </radiomedium>
    [MOTES_XML]
  </simulation>
  <plugin>
    org.contikios.cooja.plugins.ScriptRunner
    <plugin_config>
      <script><![CDATA[
TIMEOUT([TIMEOUT_MS], log.log("PIPELINE_EVENT,SIMULATION_TIMEOUT=1\n"); log.testFailed());

while (true) {
  YIELD();

  log.log(time + " ID:" + id + " " + msg + "\n");

  /* Successful completion is controlled exclusively by the client.
   * A client-side failure does NOT call testOK(); Cooja keeps running
   * until a later client success or the global TIMEOUT. */
  if (msg.indexOf("EXPERIMENT_DONE_CLIENT,success=1") >= 0) {
    log.log("PIPELINE_EVENT,CLIENT_SUCCESS_AUTHORITY=1\n");
    log.testOK();
  }

  /* Diagnostic only: never used as a completion condition. */
  if (msg.indexOf("EXPERIMENT_DONE_CLIENT,success=0") >= 0) {
    log.log("PIPELINE_EVENT,CLIENT_FAILURE_OBSERVED=1\n");
  }

  if (msg.indexOf("EXPERIMENT_DONE_SERVER") >= 0) {
    log.log("PIPELINE_EVENT,SERVER_DONE_OBSERVED=1\n");
  }
}
      ]]></script>
      <active>true</active>
    </plugin_config>
  </plugin>
</simconf>
