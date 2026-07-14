<script>
  let status = $state({ loading: true, provisioned: false, connected: false, hasWifi: false, ssid: '', apSsid: '' });
  let saving = $state(false);
  let message = $state('');
  let changeWifi = $state(false);
  let authenticated = $state(false);
  let csrf = $state('');
  let config = $state(null);

  async function loadDashboard() {
    try {
      const sessionResponse = await fetch('/api/v1/session', { cache: 'no-store' });
      if (!sessionResponse.ok) return;
      csrf = (await sessionResponse.json()).csrf;
      const configResponse = await fetch('/api/v1/config', { cache: 'no-store' });
      if (!configResponse.ok) return;
      config = await configResponse.json();
      authenticated = true;
    } catch { authenticated = false; }
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

  loadStatus();
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
    <section class="stats"><article><span class:good={status.connected}></span><p>Network</p><strong>{status.connected ? 'Online' : 'Offline'}</strong><small>{config?.ssid}</small></article><article><p>Controller state</p><strong>Safe / stopped</strong><small>Relay disabled</small></article><article><p>Setup access</p><strong>{status.apSsid}</strong><small>Fallback AP active</small></article></section>
    <section class="card"><div class="section-title"><span>IO</span><div><h3>Configured hardware</h3><p>Secrets are never returned by the API.</p></div></div><dl class="settings"><div><dt>Relay GPIO</dt><dd>{config?.relayGpio} · {config?.relayActiveHigh ? 'active high' : 'active low'}</dd></div><div><dt>Pulse</dt><dd>{config?.pulseMs} ms</dd></div><div><dt>Sensor GPIO</dt><dd>{config?.sensorGpio} · {config?.sensorActiveHigh ? 'active high' : 'active low'}</dd></div><div><dt>Travel</dt><dd>{config?.openingSeconds}s open / {config?.closingSeconds}s close</dd></div></dl></section>
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

      <section class="card"><div class="section-title"><span>04</span><div><h3>Hardware</h3><p>Choose distinct, safe ESP32 pins.</p></div></div><div class="grid three"><label>Relay GPIO<input name="relayGpio" type="number" min="0" max="39" required /></label><label>Relay active level<select name="relayLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>Pulse duration<input name="pulseMs" type="number" min="100" max="2000" value="500" required /><small>milliseconds</small></label><label>Sensor GPIO<input name="sensorGpio" type="number" min="0" max="39" required /></label><label>Closed level<select name="sensorLevel"><option value="low">Active low</option><option value="high">Active high</option></select></label><label>Sensor pull<select name="sensorPull"><option value="up">Pull-up</option><option value="down">Pull-down</option><option value="none">None</option></select></label></div></section>

      <section class="card"><div class="section-title"><span>05</span><div><h3>Travel timing</h3><p>Expected full gate movement duration.</p></div></div><div class="grid"><label>Opening time<input name="openingSeconds" type="number" min="3" max="180" value="20" required /><small>seconds</small></label><label>Closing time<input name="closingSeconds" type="number" min="3" max="180" value="20" required /><small>seconds</small></label></div></section>

      {#if message}<p class="message" role="status">{message}</p>{/if}
      <button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Validate, save & restart'}<span>→</span></button>
    </form>
  {/if}
</main>

<footer>Local configuration · No cloud connection · Relay disabled</footer>
