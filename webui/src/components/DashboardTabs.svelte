<script>
  const tabs = [['status', 'Status'], ['access', 'Access'], ['network', 'Network'], ['gate', 'Gate'], ['backup', 'Backup'], ['firmware', 'Firmware'], ['logs', 'Logs']];
  let { activeTab, onSelect } = $props();
</script>

<nav class="tabs" aria-label="Controller settings">
  {#each tabs as tab}
    <button class:active={activeTab === tab[0]} onclick={() => onSelect(tab[0])}>{tab[1]}</button>
  {/each}
</nav>
