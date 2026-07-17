<script>
  let { status, config, saving, onSave } = $props();
</script>

<section class="card">
  <div class="section-title"><span>WF</span><div><h3>Wi-Fi network</h3><p>Current network: <strong>{config?.ssid}</strong></p></div></div>
  <form onsubmit={(event) => { event.preventDefault(); onSave(event); }}><div class="grid"><label>New Wi-Fi name<input name="ssid" maxlength="32" required autocomplete="off" /></label><label>New Wi-Fi password<input name="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" /><small>Leave empty only for an open network.</small></label><label class="wide">Administrator password<input name="adminPassword" type="password" required autocomplete="current-password" /><small>Required to authorize a network migration.</small></label></div><div class="warning">The controller restarts after saving. If the new network cannot be reached, connect to <strong>{status.apSsid}</strong> and open 192.168.4.1.</div><button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Save network & restart'}<span>→</span></button></form>
</section>
<section class="card muted-card"><div class="section-title"><span>IP</span><div><h3>IP configuration</h3><p>DHCP is currently automatic. Static IP, DNS, and hostname controls require a future schema migration.</p></div><span class="badge">DHCP</span></div></section>
