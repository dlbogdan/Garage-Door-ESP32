<script>
  let { firmware, firmwareFile = $bindable(), otaUploading, otaProgress, onUpload, onSelectFile } = $props();
</script>

<section class="card">
  <div class="section-title"><span>FW</span><div><h3>Firmware update</h3><p>A/B application update with automatic boot rollback.</p></div><span class="badge">{firmware?.phase || 'loading'}</span></div>
  <dl class="settings"><div><dt>Current version</dt><dd>{firmware?.version || '—'}</dd></div><div><dt>Running partition</dt><dd>{firmware?.runningPartition || '—'}</dd></div><div><dt>Update partition</dt><dd>{firmware?.updatePartition || '—'}</dd></div><div><dt>Maximum image</dt><dd>{firmware ? `${Math.floor(firmware.maximumImageSize / 1024)} KiB` : '—'}</dd></div><div><dt>Rollback</dt><dd>{firmware?.rollbackEnabled ? 'Enabled' : 'Unavailable'}</dd></div></dl>
  <div class="warning">The gate must be stopped with no relay pulse active. Commands are interlocked during upload. Upload only a Garage-Door-ESP32 OTA application binary—not a factory image, bootloader, partition table, or filesystem image. Do not interrupt power.</div>
  <form onsubmit={onUpload}>
    <div class="grid"><label class="wide">OTA firmware (.bin)<input type="file" accept=".bin,application/octet-stream" required onchange={(event) => { firmwareFile = event.currentTarget.files?.[0] || null; onSelectFile(firmwareFile); }} /></label><label class="wide">Administrator password<input name="adminPassword" type="password" required autocomplete="current-password" /><small>Required again to authorize this firmware operation.</small></label></div>
    {#if otaUploading}<p class="message">Uploading and verifying… {otaProgress}%</p>{/if}
    <button class="primary" disabled={otaUploading || !firmware?.rollbackEnabled}>{otaUploading ? `Uploading ${otaProgress}%` : 'Upload, verify & restart'}<span>→</span></button>
  </form>
</section>
