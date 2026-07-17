<script>
  let { status, message, saving, onSave, onRestore } = $props();
  let mode = $state('new');
  let restoreFile = $state(null);
</script>

<section class="card">
  <div class="section-title"><span>01</span><div><h3>Choose setup path</h3><p>Configure this board as new, or recover a complete previous installation.</p></div></div>
  <div class="actions"><button type="button" class:primary={mode === 'new'} onclick={() => mode = 'new'}>Configure as new</button><button type="button" class:primary={mode === 'restore'} onclick={() => mode = 'restore'}>Restore full backup</button></div>
</section>

{#if mode === 'new'}
<form onsubmit={(event) => { event.preventDefault(); onSave(event); }}>
  <section class="intro">
    <div><p class="eyebrow">First-time setup</p><h2>Connect and secure</h2><p>Enter only Wi-Fi and the administrator password. Gate hardware and Apple Home settings use safe defaults and can be changed in the main app.</p></div>
    <div class="step">1 <span>of</span> 1</div>
  </section>
  <section class="card">
    <div class="section-title"><span>01</span><div><h3>Wi-Fi and administrator</h3><p>Connect the controller and protect its management page.</p></div></div>
    <div class="grid">
      <label>Wi-Fi name<input name="ssid" maxlength="32" value={status.ssid} required autocomplete="off" /></label>
      <label>Wi-Fi password<input name="password" type="password" minlength="8" maxlength="63" autocomplete="new-password" /><small>Leave empty only for an open network.</small></label>
      <label class="wide">Administrator password<input name="adminPassword" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /><small>At least 10 characters.</small></label>
    </div>
  </section>
  {#if message}<p class="message" role="status">{message}</p>{/if}
  <button class="primary" disabled={saving}>{saving ? 'Saving…' : 'Save setup & restart'}<span>→</span></button>
</form>
{:else}
<form onsubmit={(event) => { event.preventDefault(); onRestore(event, restoreFile); }}>
  <section class="intro"><div><p class="eyebrow">Disaster recovery</p><h2>Restore previous controller</h2><p>No new Wi-Fi or administrator settings are needed. They will be recovered from the backup.</p></div></section>
  <section class="card">
    <div class="grid">
      <label>Encrypted full backup<input type="file" accept=".gdbak,application/octet-stream" required onchange={(event) => restoreFile = event.currentTarget.files?.[0] || null} /></label>
      <label>Old administrator password<input name="password" type="password" minlength="10" maxlength="128" required autocomplete="off" /></label>
    </div>
    <p><small>The setup network will disappear after restore. The board will restart using Wi-Fi and Apple Home identity from the backup.</small></p>
  </section>
  {#if message}<p class="message" role="status">{message}</p>{/if}
  <button class="danger" disabled={saving}>{saving ? 'Validating…' : 'Restore full backup & restart'}</button>
</form>
{/if}
