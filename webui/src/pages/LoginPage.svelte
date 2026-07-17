<script>
  let { message, saving, onLogin } = $props();
</script>

<section class="card success">
  <div class="success-icon">⌁</div>
  <p class="eyebrow">Administrator access</p>
  <h2>Sign in to manage</h2>
  <p>Your configuration is protected by the administrator password created during setup.</p>
  <form class="login" onsubmit={(event) => { event.preventDefault(); onLogin(event); }}>
    <label>Password<input name="password" type="password" required autocomplete="current-password" /></label>
    {#if message}<p class="message">{message}</p>{/if}
    <button class="primary" disabled={saving}>{saving ? 'Signing in…' : 'Sign in'}<span>→</span></button>
  </form>
</section>
