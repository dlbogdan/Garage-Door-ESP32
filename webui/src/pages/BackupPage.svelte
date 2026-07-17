<script>
  let { saving, onBackup, onRestore } = $props();
  let restoreFile = $state(null);
</script>

<section class="card">
  <div class="section-title"><span>01</span><div><h3>Full disaster-recovery backup</h3><p>Download an encrypted copy of all device state, including Wi-Fi and Apple Home pairings.</p></div></div>
  <form onsubmit={(event) => { event.preventDefault(); onBackup(event); }}>
    <label>Current administrator password<input name="password" type="password" minlength="10" maxlength="128" required autocomplete="current-password" /></label>
    <p><small>The file can only be restored with this password. Store both safely.</small></p>
    <button class="primary" disabled={saving}>Download encrypted full backup</button>
  </form>
</section>

<section class="card">
  <div class="section-title"><span>02</span><div><h3>Restore full backup</h3><p>Replace every setting on this controller with an encrypted backup.</p></div></div>
  <form onsubmit={(event) => { event.preventDefault(); onRestore(event, restoreFile); }}>
    <label>Backup file<input type="file" accept=".gdbak,application/octet-stream" required onchange={(event) => restoreFile = event.currentTarget.files?.[0] || null} /></label>
    <label>Backup's old administrator password<input name="password" type="password" minlength="10" maxlength="128" required autocomplete="off" /></label>
    <p class="message"><strong>Warning:</strong> Current Wi-Fi, administrator, Gate, and Apple Home state will be replaced. Never run two boards restored from the same backup.</p>
    <button class="danger" disabled={saving}>Validate, stage, and restart</button>
  </form>
</section>
