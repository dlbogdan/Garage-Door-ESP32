<script>
  let { status, config } = $props();
</script>

<section class="stats">
  <article><span class:good={status.connected}></span><p>Network</p><strong>{status.connected ? 'Online' : 'Offline'}</strong><small>{config?.ssid}</small></article>
  <article><span class:good={config?.feedbackActive}></span><p>Gate feedback</p><strong>{config?.feedbackActive ? 'Active' : 'Inactive'}</strong><small>{config?.hardwareMonitoring ? 'Endpoint stability filter active' : 'Monitor unavailable'}</small></article>
  <article><p>Setup access</p><strong>{status.apSsid}</strong><small>Fallback AP active</small></article>
</section>
<section class="card">
  <div class="section-title"><span>ST</span><div><h3>System status</h3><p>Current firmware milestone and safety posture.</p></div></div>
  <dl class="settings"><div><dt>Configuration</dt><dd>Valid and protected</dd></div><div><dt>Relay output</dt><dd>{config?.relayControlEnabled ? 'Enabled' : 'Inactive · commands disabled'}</dd></div><div><dt>Sensor monitor</dt><dd>{config?.hardwareMonitoring ? 'Running' : 'Unavailable'}</dd></div><div><dt>ESP chip temperature</dt><dd>{Number.isFinite(config?.chipTemperatureCelsius) ? `${config.chipTemperatureCelsius.toFixed(1)} °C` : 'Unavailable'}</dd></div><div><dt>Fallback recovery</dt><dd>Always available</dd></div></dl>
</section>
