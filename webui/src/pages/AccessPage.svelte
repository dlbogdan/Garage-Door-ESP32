<script>
  let { homekit, config, saving, onChangePassword, onResetHomeKitPairings } = $props();
</script>

<section class="card">
  <div class="section-title"><span>PW</span><div><h3>Administrator password</h3><p>Changing it signs out every active browser session.</p></div></div>
  <form onsubmit={(event) => { event.preventDefault(); onChangePassword(event); }}><div class="grid"><label class="wide">Current password<input name="currentPassword" type="password" required autocomplete="current-password" /></label><label>New password<input name="newPassword" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /></label><label>Confirm new password<input name="confirmation" type="password" minlength="10" maxlength="128" required autocomplete="new-password" /></label></div><button class="primary" disabled={saving}>{saving ? 'Changing…' : 'Change password'}<span>→</span></button></form>
</section>
<section class="card">
  <div class="section-title"><span>HK</span><div><h3>Apple Home access</h3><p>{homekit?.paired ? 'Paired with Apple Home.' : 'Ready to pair as a Garage Door Opener.'}</p></div><span class="badge">{homekit?.paired ? 'Paired' : homekit?.active ? 'Ready' : 'Starting'}</span></div>
  <dl class="settings"><div><dt>Pairing code</dt><dd>{homekit?.setupCode || 'Loading…'}</dd></div><div><dt>Setup ID</dt><dd>{homekit?.setupId || '—'}</dd></div></dl>
  <p class="hint">On iPhone or iPad, open Home → Add Accessory → More Options. Select {config?.displayName}, then choose “Enter code” and use the pairing code above.</p>
  <div class="warning">Resetting Apple Home deletes only controller pairing records. Wi-Fi, administrator access, gate settings, and decoder rules are preserved. Use this after removing the accessory from Apple Home when it cannot be paired again.</div>
  <button type="button" class="secondary" disabled={saving || !homekit?.active} onclick={onResetHomeKitPairings}>Reset Apple Home pairings</button>
</section>
