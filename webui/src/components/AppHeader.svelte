<script>
  let {
    status,
    authenticated,
    config,
    saving,
    nameEditing = $bindable(),
    displayNameDraft = $bindable(),
    onSaveDisplayName,
    onSignOut
  } = $props();
</script>

<header class:dashboard-hero={status.provisioned && authenticated} class="hero">
  <div class="mark">GD</div>
  {#if status.provisioned && authenticated}
    <div class="identity">
      <h1>Garage-Door-ESP32</h1>
      {#if nameEditing}
        <form class="name-editor" onsubmit={onSaveDisplayName}>
          <input bind:value={displayNameDraft} maxlength="64" required aria-label="Controller display name" />
          <button disabled={saving}>Save</button>
          <button type="button" onclick={() => { displayNameDraft = config?.displayName || 'Garage'; nameEditing = false; }}>Cancel</button>
        </form>
      {:else}
        <button class="editable-name" title="Edit controller name" onclick={() => nameEditing = true}>
          {config?.displayName || 'Garage'} <span aria-hidden="true">✎</span>
        </button>
      {/if}
    </div>
  {:else}
    <div><h1>Garage-Door-ESP32</h1><p class="hero-subtitle">Controller setup</p></div>
  {/if}
  <span class:online={status.connected} class="connection">{status.connected ? 'Wi-Fi online' : 'Setup mode'}</span>
  {#if status.provisioned && authenticated}
    <button class="secondary header-signout" onclick={onSignOut}>Sign out</button>
  {/if}
</header>
