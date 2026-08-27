<?xml version="1.0" encoding="UTF-8"?>
<simconf version="2023090101">
  <project EXPORT="discard">/home/nasma/V2_Internship/code/contiki-ng/tools/cooja/apps/mspsim</project>
  <simulation>
    <title>A-kem512-fixed-prr1p00-rep01-seed200001-v2</title>
    <speedlimit>null</speedlimit>
    <randomseed>200001</randomseed>
    <motedelay_us>0</motedelay_us>
    <radiomedium>
      org.contikios.cooja.radiomediums.UDGM
      <transmitting_range>50.0</transmitting_range>
      <interference_range>100.0</interference_range>
      <success_ratio_tx>1.0</success_ratio_tx>
      <success_ratio_tx>1.0</success_ratio_tx>
<success_ratio_rx>1.00000000</success_ratio_rx>
    </radiomedium>
    <motetype>
  org.contikios.cooja.contikimote.ContikiMoteType
  <identifier>server_type</identifier>
  <description>server</description>
  <source>/home/nasma/V2_Internship/code/bench_result/Experiments/mlkem_campaign_corrected/generated/build/A-kem512-fixed-prr1p00-rep01-seed200001-v2/server.c</source>
  <commands>$(MAKE) -j$(CPUS) server.cooja TARGET=cooja</commands>
  <moteinterface>org.contikios.cooja.interfaces.Position</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiMoteID</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRS232</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.IPAddress</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRadio</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiClock</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiLED</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.Mote2MoteRelations</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.MoteAttributes</moteinterface>
</motetype>
<mote>
  <interface_config>
    org.contikios.cooja.interfaces.Position
    <pos x="57.8888" y="44.8798" />
  </interface_config>
  <interface_config>
    org.contikios.cooja.contikimote.interfaces.ContikiMoteID
    <id>1</id>
  </interface_config>
  <motetype_identifier>server_type</motetype_identifier>
</mote>
<motetype>
  org.contikios.cooja.contikimote.ContikiMoteType
  <identifier>client_type</identifier>
  <description>client</description>
  <source>/home/nasma/V2_Internship/code/bench_result/Experiments/mlkem_campaign_corrected/generated/build/A-kem512-fixed-prr1p00-rep01-seed200001-v2/client.c</source>
  <commands>$(MAKE) -j$(CPUS) client.cooja TARGET=cooja</commands>
  <moteinterface>org.contikios.cooja.interfaces.Position</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiMoteID</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRS232</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.IPAddress</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiRadio</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiClock</moteinterface>
  <moteinterface>org.contikios.cooja.contikimote.interfaces.ContikiLED</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.Mote2MoteRelations</moteinterface>
  <moteinterface>org.contikios.cooja.interfaces.MoteAttributes</moteinterface>
</motetype>
<mote>
  <interface_config>
    org.contikios.cooja.interfaces.Position
    <pos x="70.1451" y="70.6312" />
  </interface_config>
  <interface_config>
    org.contikios.cooja.contikimote.interfaces.ContikiMoteID
    <id>2</id>
  </interface_config>
  <motetype_identifier>client_type</motetype_identifier>
</mote>

  </simulation>
  <plugin>
    org.contikios.cooja.plugins.ScriptRunner
    <plugin_config>
      <script><![CDATA[
TIMEOUT(240000, log.log("PIPELINE_EVENT,SIMULATION_TIMEOUT=1\n"); log.testFailed());

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
