<script>
  let status = $state({ loading: true, provisioned: false, connected: false, hasWifi: false, ssid: '', apSsid: '' });
  let saving = $state(false);
  let message = $state('');
  let changeWifi = $state(false);
  let authenticated = $state(false);
  let csrf = $state('');
  let config = $state(null);
  let editing = $state(false);
  let activeTab = $state('status');
  let pulsing = $state(false);
  let homekit = $state(null);
  let operatorProfile = $state('sequential');
  let feedbackMode = $state('single');

  async function loadDashboard() {
    try {
      const sessionResponse = await fetch('/api/v1/session', { cache: 'no-store' });
      if (!sessionResponse.ok) return;
      csrf = (await sessionResponse.json()).csrf;
      const configResponse = await fetch('/api/v1/config', { cache: 'no-store' });
      if (!configResponse.ok) return;
      config = await configResponse.json();
      operatorProfile = config.operatorProfile || 'sequential';
      feedbackMode = config.feedbackMode || 'single';
      const homekitResponse = await fetch('/api/v1/homekit', { cache: 'no-store' });
      if (homekitResponse.ok) homekit = await homekitResponse.json();
      authenticated = true;
    } catch { authenticated = false; }
  }

  async function refreshRuntimeStatus() {
    if (!authenticated || activeTab !== 'status') return;
    try {
      const response = await fetch('/api/v1/runtime', { cache: 'no-store' });
      if (response.ok) config = { ...config, ...(await response.json()) };
      else if (response.status === 401) authenticated = false;
    } catch { /* Keep the last known state during a transient network gap. */ }
  }

  async function loadStatus() {
    try {
      const response = await fetch('/api/v1/setup/status', { cache: 'no-store' });
      status = { ...status, ...(await response.json()), loading: false };
      changeWifi = !status.hasWifi;
      if (status.provisioned) await loadDashboard();
    } catch {
      status.loading = false;
      message = 'Could not read device status. Reconnect to the setup network.';
    }
  }

  async function submit(event) {
    saving = true;
    message = 'Validating and saving configuration…';
    try {
      const data = new FormData(event.currentTarget);
      if (!changeWifi && status.hasWifi) {
        data.delete('ssid');
        data.delete('password');
      }
      const body = new URLSearchParams();
      for (const [key, value] of data) body.set(key, String(value));
      const response = await fetch('/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim());
      message = 'Saved. The controller is restarting…';
    } catch (error) {
      message = error instanceof Error ? error.message : 'Save failed.';
      saving = false;
    }
  }

  async function login(event) {
    saving = true; message = '';
    const data = new FormData(event.currentTarget);
    try {
      const response = await fetch('/api/v1/session/login', {
        method: 'POST', body: new URLSearchParams({ password: String(data.get('password') || '') })
      });
      if (!response.ok) throw new Error(await response.text());
      csrf = (await response.json()).csrf;
      await loadDashboard();
    } catch (error) { message = error instanceof Error ? error.message : 'Login failed.'; }
    saving = false;
  }

  async function mutate(path) {
    const response = await fetch(path, { method: 'POST', headers: { 'X-CSRF-Token': csrf } });
    if (!response.ok) { message = await response.text(); return; }
    if (path.endsWith('/logout')) { authenticated = false; config = null; csrf = ''; message = ''; }
    else message = 'The controller is restarting…';
  }

  async function saveSettings(event) {
    saving = true; message = 'Validating settings…';
    const data = new FormData(event.currentTarget);
    const body = new URLSearchParams();
    for (const [key, value] of data) body.set(key, String(value));
    try {
      const response = await fetch('/api/v1/config', {
        method: 'PUT', headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' }, body
      });
      if (!response.ok) throw new Error(await response.text());
      message = 'Settings saved safely.';
      editing = false;
      await loadDashboard();
    } catch (error) { message = error instanceof Error ? error.message : 'Could not save settings.'; }
    saving = false;
  }

  async function changePassword(event) {
    saving = true; message = 'Changing administrator password…';
    const form = event.currentTarget;
    const data = new FormData(form);
    const newPassword = String(data.get('newPassword') || '');
    const confirmation = String(data.get('confirmation') || '');
    if (newPassword !== confirmation) {
      message = 'New passwords do not match.'; saving = false; return;
    }
    try {
      const response = await fetch('/api/v1/access/password', {
        method: 'PUT',
        headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          currentPassword: String(data.get('currentPassword') || ''),
          newPassword,
          confirmation
        })
      });
      if (!response.ok) throw new Error(await response.text());
      form.reset();
      authenticated = false; config = null; csrf = '';
      message = 'Password changed. Sign in with the new password.';
    } catch (error) { message = error instanceof Error ? error.message : 'Could not change password.'; }
    saving = false;
  }

  async function saveWifiNetwork(event) {
    saving = true; message = 'Saving the new Wi-Fi network…';
    const data = new FormData(event.currentTarget);
    try {
      const response = await fetch('/api/v1/network/wifi', {
        method: 'PUT',
        headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          ssid: String(data.get('ssid') || ''),
          wifiPassword: String(data.get('wifiPassword') || ''),
          adminPassword: String(data.get('adminPassword') || '')
        })
      });
      if (!response.ok) throw new Error(await response.text());
      message = 'Network saved. The controller is restarting; reconnect using the new network or the fallback setup AP.';
    } catch (error) { message = error instanceof Error ? error.message : 'Could not change Wi-Fi.'; saving = false; }
  }

  async function testRelayPulse() {
    pulsing = true;
    message = 'Requesting relay pulse…';
    try {
      const response = await fetch('/api/v1/gate/test-pulse', {
        method: 'POST', headers: { 'X-CSRF-Token': csrf }
      });
      if (!response.ok) throw new Error(await response.text());
      message = `Relay pulse accepted (${config?.pulseMs} ms).`;
    } catch (error) {
      message = error instanceof Error ? error.message : 'Could not pulse relay.';
    }
    pulsing = false;
  }

  loadStatus();
  setInterval(refreshRuntimeStatus, 1000);
</script>

<svelte:head><meta name="description" content="Local Garage-Door-ESP32 configuration" /></svelte:head>

<header class="hero">
  <div class="mark">GD</div>
  <div><p class="eyebrow">Garage-Door-ESP32</p><h1>Controller setup</h1></div>
  <span class:online={status.connected} class="connection">{status.connected ? 'Wi-Fi online' : 'Setup mode'}</span>
</header>

<main>
  {#if status.loading}
    <section class="card center"><div class="spinner"></div><p>Reading device status…</p></section>
  {:else if status.provisioned && !authenticated}
    <section class="card success">
      <div class="success-icon">⌁</div><p class="eyebrow">Administrator access</p><h2>Sign in to manage</h2>
      <p>Your configuration is protected by the administrator password created during setup.</p>
      <form class="login" onsubmit={(event) => { event.preventDefault(); login(event); }}><label>Password<input name="password" type="password" required autocomplete="current-password" /></label>{#if message}<p class="message">{message}</p>{/if}<button class="primary" disabled={saving}>{saving ? 'Signing in…' : 'Sign in'}<span>→</span></button></form>
    </section>
  {:else if status.provisioned}
    <section class="intro"><div><p class="eyebrow">Management dashboard</p><h2>{config?.displayName || 'Garage controller'}</h2><p>Live connectivity and redacted device configuration.</p></div><button class="secondary" onclick={() => mutate('/api/v1/session/logout')}>Sign out</button></section>
    <nav class="tabs" aria-label="Controller settings">
      {#each [['status','Status'],['access','Access'],['network','Network'],['gate','Gate'],['logs','Logs']] as tab}
        <button class:active={activeTab === tab[0]} onclick={() => { activeTab = tab[0]; message = ''; }}>{tab[1]}</button>
      {/each}
    </nav>

    {#if activeTab === 'status'}
      <section class="stats"><article><span class:good={status.connected}></span><p>Network</p><strong>{status.connected ? 'Online' : 'Offline'}</strong><small>{config?.ssid}</small></article><article><span class:good={config?.feedbackActive}></span><p>Gate feedback</p><strong>{config?.feedbackActive ? 'Active' : 'Inactive'}</strong><small>{config?.hardwareMonitoring ? 'Endpoint stability filter active' : 'Monitor unavailable'}</small></article><article><p>Setup access</p><strong>{status.apSsid}</strong><small>Fallback AP active</small></article></section>
      <section class="card"><div class="section-title"><span>ST</span><div><h3>System status</h3><p>Current firmware milestone and safety posture.</p></div></div><dl class="settings"><div><dt>Configuration</dt><dd>Valid and protected</dd></div><div><dt>Relay output</dt><dd>{config?.relayControlEnabled ? 'Enabled' : 'Inactive · commands disabled'}</dd></div><div><dt>Sensor monitor</dt><dd>{config?.hardwareMonitoring ? 'Running' : 'Unavailable'}</dd></div><div><dt>Fallback recovery</dt><dd>Always available</dd></div></dl></section>
    {:else if activeTab === 'access'}
      <section class="card"><div class="section-title"><span>PW</span><div><h3>Administrator password</h3><p>Changing it signs out every active browser session.</p></div></div><form onsubmit={(event) => { event.preventDefault(); changePassword(event); }}><div class="grid"><label class="wide">Current password<input name="currentPassword" type="password" required autocomplete="current-password" /></label><label>New password<input name="newPassword" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /></label><label>Confirm new password<input name="confirmation" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /></label></div><button class="primary" disabled={saving}>{saving ? 'Changing…' : 'Change password'}<span>→</span></button></form></section>
      <section class="card"><div class="section-title"><span>HK</span><div><h3>Apple Home access</h3><p>{homekit?.paired ? 'Paired with Apple Home.' : 'Ready to pair as a Garage Door Opener.'}</p></div><span class="badge">{homekit?.paired ? 'Paired' : homekit?.active ? 'Ready' : 'Starting'}</span></div><dl class="settings"><div><dt>Pairing code</dt><dd>{homekit?.setupCode || 'Loading…'}</dd></div><div><dt>Setup ID</dt><dd>{homekit?.setupId || '—'}</dd></div></dl><p class="hint">On iPhone or iPad, open Home → Add Accessory → More Options. Select {config?.displayName}, then choose “Enter code” and use the pairing code above.</p></section>
    {:else if activeTab === 'network'}
      <section class="card"><div class="section-title"><span>WF</span><div><h3>Wi-Fi network</h3><p>Current network: <strong>{config?.ssid}</strong></p></div></div><form onsubmit={(event) => { event.preventDefault(); saveWifiNetwork(event); }}><div class="grid"><label>New Wi-Fi name<input name="ssid" maxlength="32" required autocomplete="off" /></label><label>New Wi-Fi password<input name="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" /><small>Leave empty only for an open network.</small></label><label class="wide">Administrator password<input name="adminPassword" type="password" required autocomplete="current-password" /><small>Required to authorize a network migration.</small></label></div><div class="warning">The controller restarts after saving. If the new network cannot be reached, connect to <strong>{status.apSsid}</strong> and open 192.168.4.1.</div><button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Save network & restart'}<span>→</span></button></form></section>
      <section class="card muted-card"><div class="section-title"><span>IP</span><div><h3>IP configuration</h3><p>DHCP is currently automatic. Static IP, DNS, and hostname controls require a future schema migration.</p></div><span class="badge">DHCP</span></div></section>
    {:else if activeTab === 'gate'}
      <section class="card"><div class="section-title"><span>IO</span><div><h3>Gate hardware & timing</h3><p>Relay, feedback sensor, pulse logic, and travel timeouts.</p></div><button class="secondary edit" onclick={() => { editing = !editing; message = ''; }}>{editing ? 'Cancel' : 'Edit settings'}</button></div>
        {#if editing}
          <div class="grid"><label>Operator profile<select bind:value={operatorProfile}><option value="sequential">One STEP input</option><option value="directional">Separate OPEN / CLOSE inputs</option></select></label><label>Feedback topology<select bind:value={feedbackMode}><option value="single">One endpoint input</option><option value="dual">Separate OPENED / CLOSED inputs</option></select></label></div>
          <div class="warning" aria-live="polite">{operatorProfile === 'sequential' ? 'Wiring: ESP32 → relay → STEP input. Opposite travel commands pause and require a later explicit command to reverse.' : 'Wiring: ESP32 → OPEN relay → OPEN input; ESP32 → CLOSE relay → CLOSE input. Opposite travel commands pulse the requested direction immediately; use only with a safely reversing operator.'} {feedbackMode === 'dual' ? 'Dual feedback: neither input is BETWEEN; both asserted is a command-interlocking contradiction.' : 'Single feedback: one configured level identifies one endpoint.'}</div>
          <form onsubmit={(event) => { event.preventDefault(); saveSettings(event); }}>
            <input type="hidden" name="operatorProfile" value={operatorProfile} />
            <input type="hidden" name="feedbackMode" value={feedbackMode} />
            <div class="grid">
              <label class="wide">Display name<input name="displayName" maxlength="64" value={config?.displayName} required /></label>
              {#if operatorProfile === 'sequential'}
                <label>STEP GPIO<input name="stepGpio" type="number" min="0" max="39" value={config?.relayGpio} required /></label><label>STEP level<select name="stepLevel" value={config?.relayActiveHigh ? 'high' : 'low'}><option value="low">Active low</option><option value="high">Active high</option></select></label><label>STEP pulse<input name="stepPulseMs" type="number" min="100" max="2000" value={config?.pulseMs} required /></label>
              {:else}
                <label>OPEN GPIO<input name="openGpio" type="number" min="0" max="39" value={config?.openGpio} required /></label><label>OPEN level<select name="openLevel" value={config?.openActiveHigh ? 'high' : 'low'}><option value="low">Active low</option><option value="high">Active high</option></select></label><label>OPEN pulse<input name="openPulseMs" type="number" min="100" max="2000" value={config?.openPulseMs || 500} required /></label>
                <label>CLOSE GPIO<input name="closeGpio" type="number" min="0" max="39" value={config?.closeGpio} required /></label><label>CLOSE level<select name="closeLevel" value={config?.closeActiveHigh ? 'high' : 'low'}><option value="low">Active low</option><option value="high">Active high</option></select></label><label>CLOSE pulse<input name="closePulseMs" type="number" min="100" max="2000" value={config?.closePulseMs || 500} required /></label>
              {/if}
              {#if feedbackMode === 'single'}
                <label>Feedback GPIO<input name="sensorGpio" type="number" min="0" max="39" value={config?.sensorGpio} required /></label><label>Feedback level<select name="sensorLevel" value={config?.sensorActiveHigh ? 'high' : 'low'}><option value="low">Active low</option><option value="high">Active high</option></select></label><label>Active means<select name="feedbackEndpoint" value={config?.feedbackActiveEndpoint}><option value="open">Gate open</option><option value="closed">Gate closed</option></select></label><label>Feedback pull<select name="sensorPull" value={config?.sensorPull}><option value="up">Pull-up</option><option value="down">Pull-down</option><option value="none">None</option></select></label>
              {:else}
                <label>OPENED GPIO<input name="openedSensorGpio" type="number" min="0" max="39" required /></label><label>OPENED level<select name="openedSensorLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>OPENED pull<select name="openedSensorPull"><option value="up">Pull-up</option><option value="down">Pull-down</option><option value="none">None</option></select></label>
                <label>CLOSED GPIO<input name="closedSensorGpio" type="number" min="0" max="39" required /></label><label>CLOSED level<select name="closedSensorLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>CLOSED pull<select name="closedSensorPull"><option value="up">Pull-up</option><option value="down">Pull-down</option><option value="none">None</option></select></label>
              {/if}
              <label>Endpoint stability<input name="feedbackStabilityMs" type="number" min="1000" max="10000" value={config?.feedbackStabilityMs} required /></label><label>Opening timeout<input name="openingSeconds" type="number" min="3" max="180" value={config?.openingSeconds} required /></label><label>Closing timeout<input name="closingSeconds" type="number" min="3" max="180" value={config?.closingSeconds} required /></label>
            </div><button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Save gate settings'}<span>→</span></button>
          </form>
        {:else}<dl class="settings"><div><dt>Relay GPIO</dt><dd>{config?.relayGpio} · {config?.relayActiveHigh ? 'active high' : 'active low'}</dd></div><div><dt>Pulse</dt><dd>{config?.pulseMs} ms</dd></div><div><dt>Sensor GPIO</dt><dd>{config?.sensorGpio} · {config?.sensorActiveHigh ? 'active high' : 'active low'}</dd></div><div><dt>Sensor pull</dt><dd>{config?.sensorPull}</dd></div><div><dt>Travel</dt><dd>{config?.openingSeconds}s open / {config?.closingSeconds}s close</dd></div></dl><div class="warning">Bench test: this energizes relay GPIO {config?.relayGpio} once for {config?.pulseMs} ms.</div><button class="primary" disabled={pulsing || !config?.relayControlEnabled} onclick={testRelayPulse}>{pulsing ? 'Pulsing…' : 'Test relay pulse'}<span>→</span></button>{/if}
      </section>
    {:else}
      <section class="card empty-state"><div class="success-icon">≡</div><h3>Event logs</h3><p>Persistent redacted event logging is not implemented yet. Runtime diagnostics remain available through the ESP32 serial monitor.</p><span class="badge">Planned</span></section>
    {/if}
    {#if message}<p class="message">{message}</p>{/if}<button class="danger" onclick={() => mutate('/api/v1/system/reboot')}>Restart controller</button>
  {:else}
    <form onsubmit={(event) => { event.preventDefault(); submit(event); }}>
      <section class="intro"><div><p class="eyebrow">Secure first-time setup</p><h2>Configure your garage controller</h2><p>Settings are validated on the ESP32 before anything is stored. Relay output stays disabled.</p></div><div class="step">1 <span>of</span> 1</div></section>

      <section class="card"><div class="section-title"><span>01</span><div><h3>Administrator</h3><p>Protect future device management.</p></div></div><label>Administrator password<input name="adminPassword" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /><small>At least 10 characters. Only a salted verifier is stored.</small></label></section>

      <section class="card"><div class="section-title"><span>02</span><div><h3>Network</h3><p>Connect the controller to your local Wi-Fi.</p></div></div>
        {#if status.hasWifi}<div class="current"><div><strong>Current network</strong><span>{status.ssid}</span></div><label class="switch"><input type="checkbox" bind:checked={changeWifi} /><span></span>Change</label></div>{/if}
        {#if changeWifi}<div class="grid"><label>Wi-Fi name<input name="ssid" maxlength="32" required={changeWifi} autocomplete="off" /></label><label>Wi-Fi password<input name="password" type="password" minlength="8" maxlength="63" required={changeWifi} autocomplete="new-password" /></label></div>{:else}<p class="hint">The saved Wi-Fi password will be reused. It is never sent to this page.</p>{/if}
      </section>

      <section class="card"><div class="section-title"><span>03</span><div><h3>Apple Home</h3><p>Identity used for Garage Door Opener pairing.</p></div></div><div class="grid"><label>Display name<input name="displayName" maxlength="64" value="Garage" required /></label><label>Setup ID<input name="setupId" pattern="[A-Z0-9]{4}" maxlength="4" placeholder="G7T2" required /></label><label class="wide">Eight-digit setup code<input name="setupCode" inputmode="numeric" pattern="[0-9]{8}" maxlength="8" placeholder="48271635" required /><small>Avoid repeated or sequential digits.</small></label></div></section>

      <section class="card"><div class="section-title"><span>04</span><div><h3>Hardware</h3><p>Choose distinct, safe ESP32 pins.</p></div></div><div class="grid three"><label>Relay GPIO<input name="relayGpio" type="number" min="0" max="39" required /></label><label>Relay active level<select name="relayLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>Pulse duration<input name="pulseMs" type="number" min="100" max="2000" value="1000" required /><small>milliseconds</small></label><label>Feedback GPIO<input name="sensorGpio" type="number" min="0" max="39" required /></label><label>Electrical active level<select name="sensorLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>Active signal means<select name="feedbackEndpoint"><option value="open">Gate open</option><option value="closed">Gate closed</option></select></label><label>Endpoint stability<input name="feedbackStabilityMs" type="number" min="1000" max="10000" value="2000" required /><small>milliseconds</small></label><label>Sensor pull<select name="sensorPull"><option value="up">Pull-up</option><option value="down">Pull-down</option><option value="none">None</option></select></label></div></section>

      <section class="card"><div class="section-title"><span>05</span><div><h3>Travel timing</h3><p>Expected full gate movement duration.</p></div></div><div class="grid"><label>Opening time<input name="openingSeconds" type="number" min="3" max="180" value="20" required /><small>seconds</small></label><label>Closing time<input name="closingSeconds" type="number" min="3" max="180" value="20" required /><small>seconds</small></label></div></section>

      {#if message}<p class="message" role="status">{message}</p>{/if}
      <button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Validate, save & restart'}<span>→</span></button>
    </form>
  {/if}
</main>

<footer>Local configuration · No cloud connection · Relay disabled</footer>
