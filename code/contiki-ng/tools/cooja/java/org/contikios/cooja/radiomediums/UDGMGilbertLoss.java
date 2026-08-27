package org.contikios.cooja.radiomediums;

import java.util.Collection;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Random;
import org.contikios.cooja.ClassDescription;
import org.contikios.cooja.RadioConnection;
import org.contikios.cooja.Simulation;
import org.contikios.cooja.interfaces.Position;
import org.contikios.cooja.interfaces.Radio;
import org.jdom2.Element;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * UDGM radio medium with a pure two-state Gilbert loss process.
 *
 * <p>A separate Markov chain is maintained for every directed radio link
 * {@code source -> destination}. A chain step is performed exactly once for
 * every radio transmission evaluated for a destination that is:</p>
 *
 * <ul>
 *   <li>different from the sender,</li>
 *   <li>on the same configured channel, and</li>
 *   <li>inside the sender's current transmission range.</li>
 * </ul>
 *
 * <p>The state used for the current attempt is sampled first, and the Markov
 * transition determines the state of the next attempt:</p>
 *
 * <pre>
 * GOOD --P_GB--> BAD
 * BAD  --P_BG--> GOOD
 * </pre>
 *
 * <p>Pure Gilbert reception rule:</p>
 * <ul>
 *   <li>GOOD: channel admits the reception;</li>
 *   <li>BAD: channel rejects the reception.</li>
 * </ul>
 *
 * <p>In stationarity:</p>
 * <pre>
 * PRR              = P_BG / (P_GB + P_BG)
 * mean BAD run     = 1 / P_BG
 * mean GOOD run    = 1 / P_GB
 * </pre>
 *
 * <p>To target a stationary PRR q and a mean loss-burst length L:</p>
 * <pre>
 * P_BG = 1 / L
 * P_GB = P_BG * (1 - q) / q
 * </pre>
 *
 * <p>Important: inherited SUCCESS_RATIO_RX is intentionally ignored. For a
 * clean Gilbert validation, configure SUCCESS_RATIO_TX = 1.0 as well.</p>
 */
@ClassDescription("UDGM: Gilbert Burst Loss")
public class UDGMGilbertLoss extends UDGM {
  private static final Logger logger =
      LoggerFactory.getLogger(UDGMGilbertLoss.class);

  /** GOOD -> BAD transition probability per evaluated radio attempt. */
  public double P_GB = 0.05;

  /** BAD -> GOOD transition probability per evaluated radio attempt. */
  public double P_BG = 0.20;

  /**
   * If true, each new directed link starts from the stationary distribution.
   * If false, each new link starts in GOOD.
   */
  public boolean INITIALIZE_STATIONARY = true;

  /** Log one line whenever a GOOD or BAD run ends. */
  public boolean LOG_BURSTS = false;

  /** Log every evaluated directed-link attempt. Intended for diagnostics. */
  public boolean LOG_EACH_ATTEMPT = false;

  /**
   * Log a cumulative summary every N attempts on each directed link.
   * A value of 0 disables periodic summaries.
   */
  public int LOG_SUMMARY_EVERY = 0;

  private enum ChannelState {
    GOOD,
    BAD
  }

  /** Immutable directed-link identifier. */
  private static final class LinkKey {
    private final int sourceId;
    private final int destinationId;

    private LinkKey(int sourceId, int destinationId) {
      this.sourceId = sourceId;
      this.destinationId = destinationId;
    }

    @Override
    public boolean equals(Object other) {
      if (this == other) {
        return true;
      }
      if (!(other instanceof LinkKey)) {
        return false;
      }
      LinkKey that = (LinkKey) other;
      return sourceId == that.sourceId
          && destinationId == that.destinationId;
    }

    @Override
    public int hashCode() {
      return Objects.hash(sourceId, destinationId);
    }

    @Override
    public String toString() {
      return sourceId + "->" + destinationId;
    }
  }

  /** State and validation statistics for one directed link. */
  private static final class LinkStats {
    private ChannelState state;
    private long currentRunLength;

    private long attempts;
    private long goodAttempts;
    private long badAttempts;
    private long rxStarted;

    private long nGG;
    private long nGB;
    private long nBG;
    private long nBB;

    private long completedGoodRuns;
    private long completedBadRuns;
    private long sumGoodRunLengths;
    private long sumBadRunLengths;

    private LinkStats(ChannelState initialState) {
      state = initialState;
    }
  }

  /** Result of one Gilbert evaluation before receiver-side constraints. */
  private static final class ChannelSample {
    private final LinkKey key;
    private final LinkStats stats;
    private final ChannelState current;
    private final ChannelState next;
    private final double draw;
    private final double threshold;
    private final boolean channelAllowsReception;

    private ChannelSample(
        LinkKey key,
        LinkStats stats,
        ChannelState current,
        ChannelState next,
        double draw,
        double threshold,
        boolean channelAllowsReception) {
      this.key = key;
      this.stats = stats;
      this.current = current;
      this.next = next;
      this.draw = draw;
      this.threshold = threshold;
      this.channelAllowsReception = channelAllowsReception;
    }
  }

  private final Map<LinkKey, LinkStats> links = new HashMap<>();
  private final Random random;

  public UDGMGilbertLoss(Simulation simulation) {
    super(simulation);
    random = simulation.getRandomGenerator();
  }
  /** Two channels conflict only if both are configured (>=0) and differ. */
  private static boolean channelsDiffer(int senderChannel, int receiverChannel) {
    return senderChannel >= 0 && receiverChannel >= 0 && senderChannel != receiverChannel;
  }
  /**
   * Reimplements UDGM's connection decision so that the Gilbert chain is
   * advanced at the actual transmission-evaluation point, rather than inside
   * a probability getter that could also be called by a GUI or plugin.
   */
  @Override
  protected RadioConnection createConnections(Radio sender) {
    validateRuntimeConfiguration();

    RadioConnection connection = new RadioConnection(sender);

    /* Optional global TX loss inherited from UDGM. Keep this at 1.0 when
     * validating the Gilbert formulas in isolation. */
    double txSuccessProbability = getTxSuccessProbability();
    if (txSuccessProbability < 1.0
        && random.nextDouble() >= txSuccessProbability) {
      return connection;
    }

    double transmissionRange = scaledRange(sender, TRANSMITTING_RANGE);
    double interferenceRange = scaledRange(sender, INTERFERENCE_RANGE);
    double maximumRelevantRange =
        Math.max(transmissionRange, interferenceRange);

    Position senderPosition = sender.getPosition();

    for (Radio receiver : getRegisteredRadios()) {
      if (receiver == sender) {
        continue;
      }

      double distance =
          senderPosition.getDistanceTo(receiver.getPosition());
      if (distance > maximumRelevantRange) {
        continue;
      }

      /* Preserve UDGM's channel behavior. A negative channel denotes all
       * channels, as handled by channelsDiffer(). */
      if (channelsDiffer(sender.getChannel(), receiver.getChannel())) {
        connection.addInterfered(receiver);
        continue;
      }

      if (transmissionRange > 0.0 && distance <= transmissionRange) {
        /* Exactly one Markov step for this in-range directed-link attempt. */
        ChannelSample sample = sampleAndAdvance(sender, receiver);

        boolean receptionStarted = false;
        String result;

        if (!receiver.isRadioOn()) {
          connection.addInterfered(receiver);
          receiver.interfereAnyReception();
          result = "RADIO_OFF";

        } else if (receiver.isInterfered()) {
          connection.addInterfered(receiver);
          result = "ALREADY_INTERFERED";

        } else if (receiver.isTransmitting()) {
          connection.addInterfered(receiver);
          receiver.interfereAnyReception();
          result = "RECEIVER_TRANSMITTING";

        } else {
          boolean receiverWasBusy = receiver.isReceiving();

          if (receiverWasBusy || !sample.channelAllowsReception) {
            connection.addInterfered(receiver);
            receiver.interfereAnyReception();

            /* Interfere any older active reception at this receiver, as UDGM
             * does when a collision or reception failure occurs. */
            for (RadioConnection active : getActiveConnections()) {
              if (active.isDestination(receiver)) {
                active.addInterfered(receiver);
              }
            }

            if (receiverWasBusy && !sample.channelAllowsReception) {
              result = "RECEIVER_BUSY_AND_GILBERT_BAD";
            } else if (receiverWasBusy) {
              result = "RECEIVER_BUSY";
            } else {
              result = "GILBERT_BAD";
            }

          } else {
            connection.addDestination(receiver);
            receptionStarted = true;
            result = "RX_STARTED";
          }
        }

        finishAttempt(sample, receptionStarted, result);

      } else if (interferenceRange > 0.0
          && distance <= interferenceRange) {
        connection.addInterfered(receiver);
        receiver.interfereAnyReception();
      }
    }

    return connection;
  }

  /**
   * Read-only probability view for GUI/plugins. This method never advances
   * the Markov chain.
   */
  @Override
  public double getRxSuccessProbability(Radio source, Radio destination) {
    double transmissionRange = scaledRange(source, TRANSMITTING_RANGE);
    if (transmissionRange <= 0.0) {
      return 0.0;
    }

    double distance = source.getPosition()
        .getDistanceTo(destination.getPosition());
    if (distance > transmissionRange) {
      return 0.0;
    }

    LinkStats stats = links.get(linkKey(source, destination));
    if (stats == null) {
      return stationaryGoodProbability();
    }
    return stats.state == ChannelState.GOOD ? 1.0 : 0.0;
  }

  private ChannelSample sampleAndAdvance(Radio source, Radio destination) {
    LinkKey key = linkKey(source, destination);
    LinkStats stats = links.computeIfAbsent(
        key, ignored -> new LinkStats(initialState()));

    ChannelState current = stats.state;
    boolean channelAllowsReception = current == ChannelState.GOOD;

    stats.attempts++;
    stats.currentRunLength++;
    if (channelAllowsReception) {
      stats.goodAttempts++;
    } else {
      stats.badAttempts++;
    }

    double draw = random.nextDouble();
    double threshold = current == ChannelState.GOOD ? P_GB : P_BG;

    ChannelState next;
    if (current == ChannelState.GOOD) {
      next = draw < P_GB ? ChannelState.BAD : ChannelState.GOOD;
      if (next == ChannelState.BAD) {
        stats.nGB++;
      } else {
        stats.nGG++;
      }
    } else {
      next = draw < P_BG ? ChannelState.GOOD : ChannelState.BAD;
      if (next == ChannelState.GOOD) {
        stats.nBG++;
      } else {
        stats.nBB++;
      }
    }

    if (current != next) {
      completeRun(key, stats, current);
      stats.currentRunLength = 0;
    }

    stats.state = next;

    return new ChannelSample(
        key,
        stats,
        current,
        next,
        draw,
        threshold,
        channelAllowsReception);
  }

  private void completeRun(
      LinkKey key, LinkStats stats, ChannelState completedState) {
    long length = stats.currentRunLength;

    if (completedState == ChannelState.GOOD) {
      stats.completedGoodRuns++;
      stats.sumGoodRunLengths += length;
    } else {
      stats.completedBadRuns++;
      stats.sumBadRunLengths += length;
    }

    if (LOG_BURSTS) {
      logger.info(
          "[Gilbert] RUN link={} state={} length={} attempts "
              + "N_GG={} N_GB={} N_BG={} N_BB={}",
          key,
          completedState,
          length,
          stats.nGG,
          stats.nGB,
          stats.nBG,
          stats.nBB);
    }
  }

  private void finishAttempt(
      ChannelSample sample, boolean receptionStarted, String result) {
    if (receptionStarted) {
      sample.stats.rxStarted++;
    }

    if (LOG_EACH_ATTEMPT) {
      logger.info(
          "[Gilbert] ATTEMPT link={} index={} current={} draw={} "
              + "threshold={} next={} channel_ok={} rx_started={} result={}",
          sample.key,
          sample.stats.attempts,
          sample.current,
          sample.draw,
          sample.threshold,
          sample.next,
          sample.channelAllowsReception,
          receptionStarted,
          result);
    }

    if (LOG_SUMMARY_EVERY > 0
        && sample.stats.attempts % LOG_SUMMARY_EVERY == 0) {
      logSummary(sample.key, sample.stats);
    }
  }

  private void logSummary(LinkKey key, LinkStats stats) {
    double empiricalChannelPrr = divide(stats.goodAttempts, stats.attempts);
    double rxStartRatio = divide(stats.rxStarted, stats.attempts);

    double pGbHat = divide(stats.nGB, stats.nGG + stats.nGB);
    double pBgHat = divide(stats.nBG, stats.nBB + stats.nBG);

    double meanGoodRun =
        divide(stats.sumGoodRunLengths, stats.completedGoodRuns);
    double meanBadRun =
        divide(stats.sumBadRunLengths, stats.completedBadRuns);

    logger.info(
        "[Gilbert] SUMMARY link={} attempts={} good={} bad={} "
            + "channel_prr_hat={} rx_started={} rx_start_ratio={} "
            + "N_GG={} N_GB={} N_BG={} N_BB={} "
            + "P_GB_hat={} P_BG_hat={} mean_good_run={} "
            + "mean_bad_run={} current_state={} open_run_length={}",
        key,
        stats.attempts,
        stats.goodAttempts,
        stats.badAttempts,
        empiricalChannelPrr,
        stats.rxStarted,
        rxStartRatio,
        stats.nGG,
        stats.nGB,
        stats.nBG,
        stats.nBB,
        pGbHat,
        pBgHat,
        meanGoodRun,
        meanBadRun,
        stats.state,
        stats.currentRunLength);
  }

  /** Log the current cumulative summary for every known directed link. */
  public void logAllSummaries() {
    for (Map.Entry<LinkKey, LinkStats> entry : links.entrySet()) {
      logSummary(entry.getKey(), entry.getValue());
    }
  }

  /** Reset all Markov states and all collected statistics. */
  public void resetGilbertState() {
    links.clear();
  }

  private LinkKey linkKey(Radio source, Radio destination) {
    return new LinkKey(
        source.getMote().getID(), destination.getMote().getID());
  }

  private ChannelState initialState() {
    if (!INITIALIZE_STATIONARY) {
      return ChannelState.GOOD;
    }

    double sum = P_GB + P_BG;
    if (sum == 0.0) {
      /* No unique stationary distribution. Choose GOOD deterministically. */
      return ChannelState.GOOD;
    }

    return random.nextDouble() < stationaryGoodProbability()
        ? ChannelState.GOOD
        : ChannelState.BAD;
  }

  private double stationaryGoodProbability() {
    double sum = P_GB + P_BG;
    return sum == 0.0 ? 1.0 : P_BG / sum;
  }

  private static double scaledRange(Radio source, double configuredRange) {
    int maximumPower = source.getOutputPowerIndicatorMax();
    if (configuredRange <= 0.0 || maximumPower <= 0) {
      return 0.0;
    }

    return configuredRange
        * ((double) source.getCurrentOutputPowerIndicator()
            / (double) maximumPower);
  }

  private static double divide(long numerator, long denominator) {
    return denominator == 0
        ? Double.NaN
        : (double) numerator / (double) denominator;
  }

  private void validateRuntimeConfiguration() {
    validateProbability("P_GB", P_GB);
    validateProbability("P_BG", P_BG);
    validateProbability("SUCCESS_RATIO_TX", getTxSuccessProbability());

    if (LOG_SUMMARY_EVERY < 0) {
      throw new IllegalStateException(
          "LOG_SUMMARY_EVERY must be >= 0: " + LOG_SUMMARY_EVERY);
    }
  }

  private static void validateProbability(String name, double value) {
    if (!Double.isFinite(value) || value < 0.0 || value > 1.0) {
      throw new IllegalStateException(
          name + " must be finite and in [0, 1], got " + value);
    }
  }

  @Override
  public Collection<Element> getConfigXML() {
    Collection<Element> config = super.getConfigXML();

    addElement(config, "gilbert_p_gb", P_GB);
    addElement(config, "gilbert_p_bg", P_BG);
    addElement(config, "gilbert_initialize_stationary",
        INITIALIZE_STATIONARY);
    addElement(config, "gilbert_log_bursts", LOG_BURSTS);
    addElement(config, "gilbert_log_each_attempt", LOG_EACH_ATTEMPT);
    addElement(config, "gilbert_log_summary_every", LOG_SUMMARY_EVERY);

    return config;
  }

  @Override
  public boolean setConfigXML(
      Collection<Element> configXML, boolean visAvailable) {
    if (!super.setConfigXML(configXML, visAvailable)) {
      return false;
    }

    boolean foundPgb = false;
    boolean foundPbg = false;

    try {
      for (Element element : configXML) {
        switch (element.getName()) {
          case "gilbert_p_gb":
            P_GB = Double.parseDouble(element.getTextTrim());
            foundPgb = true;
            break;

          case "gilbert_p_bg":
            P_BG = Double.parseDouble(element.getTextTrim());
            foundPbg = true;
            break;

          case "gilbert_initialize_stationary":
            INITIALIZE_STATIONARY =
                Boolean.parseBoolean(element.getTextTrim());
            break;

          case "gilbert_log_bursts":
            LOG_BURSTS = Boolean.parseBoolean(element.getTextTrim());
            break;

          case "gilbert_log_each_attempt":
            LOG_EACH_ATTEMPT =
                Boolean.parseBoolean(element.getTextTrim());
            break;

          case "gilbert_log_summary_every":
            LOG_SUMMARY_EVERY = Integer.parseInt(element.getTextTrim());
            break;

          default:
            /* Elements owned by UDGM or AbstractRadioMedium. */
            break;
        }
      }

      validateRuntimeConfiguration();

    } catch (IllegalArgumentException | IllegalStateException exception) {
      logger.error("[Gilbert] Invalid configuration", exception);
      return false;
    }

    resetGilbertState();

    double theoreticalPrr = stationaryGoodProbability();
    double expectedBadRun = P_BG == 0.0
        ? Double.POSITIVE_INFINITY
        : 1.0 / P_BG;
    double expectedGoodRun = P_GB == 0.0
        ? Double.POSITIVE_INFINITY
        : 1.0 / P_GB;

    logger.info(
        "[Gilbert] CONFIG P_GB={} (found={}) P_BG={} (found={}) "
            + "stationary_init={} LOG_BURSTS={} LOG_EACH_ATTEMPT={} "
            + "LOG_SUMMARY_EVERY={} theoretical_PRR={} "
            + "E_good_run={} E_bad_run={} SUCCESS_RATIO_TX={} "
            + "SUCCESS_RATIO_RX={} (ignored)",
        P_GB,
        foundPgb,
        P_BG,
        foundPbg,
        INITIALIZE_STATIONARY,
        LOG_BURSTS,
        LOG_EACH_ATTEMPT,
        LOG_SUMMARY_EVERY,
        theoreticalPrr,
        expectedGoodRun,
        expectedBadRun,
        getTxSuccessProbability(),
        SUCCESS_RATIO_RX);

    if (getTxSuccessProbability() != 1.0) {
      logger.warn(
          "[Gilbert] SUCCESS_RATIO_TX is not 1.0; observed reception "
              + "will combine global TX loss with Gilbert channel loss");
    }

    return true;
  }

  private static void addElement(
      Collection<Element> config, String name, Object value) {
    Element element = new Element(name);
    element.setText(String.valueOf(value));
    config.add(element);
  }
}