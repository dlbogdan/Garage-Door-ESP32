<script>
  import { appendTrace, consistentLearningIntervals, inputReferenceCount, nextAssignedId, runtimePollMode, traceInputs } from './gate-view.js';
  import AppHeader from './components/AppHeader.svelte';
  import DashboardTabs from './components/DashboardTabs.svelte';
  import LoginPage from './pages/LoginPage.svelte';
  import SetupPage from './pages/SetupPage.svelte';
  import StatusPage from './pages/StatusPage.svelte';
  import AccessPage from './pages/AccessPage.svelte';
  import NetworkPage from './pages/NetworkPage.svelte';
  import FirmwarePage from './pages/FirmwarePage.svelte';
  import LogsPage from './pages/LogsPage.svelte';
  import GatePage from './pages/GatePage.svelte';
  import BackupPage from './pages/BackupPage.svelte';

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
  let feedbackDecoder = $state('customRules');
  let decoder = $state({ inputs: [], rules: [], limits: { inputs: 4, rules: 8, groupsPerRule: 2, predicatesPerGroup: 3 } });
  let firmware = $state(null);
  let firmwareFile = $state(null);
  let otaUploading = $state(false);
  let otaProgress = $state(0);
  let learningPredicate = $state(null);
  let learningProgress = $state(0);
  let learningTrace = $state([]);
  let traceHistory = $state([]);
  let traceNow = $state(Date.now());
  let runtimeRequestActive = false;
  let lastStatusRefresh = 0;
  let liveInputs = $derived(traceInputs(config, decoder));
  let nameEditing = $state(false);
  let displayNameDraft = $state('');
  let editingPatternIndex = $state(null);
  let gateProfileReview = $state(null);

  async function loadDashboard() {
    try {
      const sessionResponse = await fetch('/api/v1/session', { cache: 'no-store' });
      if (!sessionResponse.ok) return;
      csrf = (await sessionResponse.json()).csrf;
      const configResponse = await fetch('/api/v1/config', { cache: 'no-store' });
      if (!configResponse.ok) return;
      config = await configResponse.json();
      displayNameDraft = config.displayName || 'Garage';
      operatorProfile = config.operatorProfile || 'sequential';
      feedbackDecoder = 'customRules';
      decoder = config.decoder || decoder;
      const homekitResponse = await fetch('/api/v1/homekit', { cache: 'no-store' });
      if (homekitResponse.ok) homekit = await homekitResponse.json();
      authenticated = true;
      await loadFirmware();
    } catch { authenticated = false; }
  }

  async function refreshRuntimeStatus() {
    if (runtimeRequestActive) return;
    const now = Date.now();
    const pollMode = runtimePollMode({ authenticated, hidden: document.hidden, activeTab, editing, elapsed: now - lastStatusRefresh });
    if (pollMode === 'none') return;
    runtimeRequestActive = true;
    try {
      const response = await fetch('/api/v1/runtime', { cache: 'no-store' });
      if (response.ok) {
        config = { ...config, ...(await response.json()) };
        if (pollMode === 'gate') {
          traceNow = now;
          traceHistory = appendTrace(traceHistory, traceInputs(config, decoder), now);
        } else lastStatusRefresh = now;
      }
      else if (response.status === 401) authenticated = false;
    } catch { /* Keep the last known state during a transient network gap. */ }
    finally { runtimeRequestActive = false; }
  }

  async function loadFirmware() {
    if (!authenticated) return;
    try {
      const response = await fetch('/api/v1/system/firmware', { cache: 'no-store' });
      if (response.ok) firmware = await response.json();
    } catch { /* Reconnection after a successful update is expected. */ }
  }

  function uploadFirmware(event) {
    event.preventDefault();
    const form = event.currentTarget;
    const data = new FormData(form);
    const file = firmwareFile;
    const adminPassword = String(data.get('adminPassword') || '');
    if (!file || !adminPassword) { message = 'Select firmware and enter the administrator password.'; return; }
    if (file.size > (firmware?.maximumImageSize || 0)) { message = 'Firmware is larger than the OTA application slot.'; return; }
    otaUploading = true; otaProgress = 0; message = 'Uploading firmware. Do not interrupt power…';
    const request = new XMLHttpRequest();
    request.open('POST', '/api/v1/system/firmware');
    request.setRequestHeader('Content-Type', 'application/octet-stream');
    request.setRequestHeader('X-CSRF-Token', csrf);
    request.setRequestHeader('X-Admin-Password', adminPassword);
    request.upload.onprogress = (progress) => {
      if (progress.lengthComputable) otaProgress = Math.round(progress.loaded * 100 / progress.total);
    };
    request.onload = () => {
      otaUploading = false;
      if (request.status >= 200 && request.status < 300) {
        otaProgress = 100; message = 'Firmware verified. The controller is restarting…'; form.reset(); firmwareFile = null;
      } else message = request.responseText || 'Firmware update failed.';
      loadFirmware();
    };
    request.onerror = () => { otaUploading = false; message = 'Upload connection failed.'; loadFirmware(); };
    request.send(file);
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
      if (!data.has('displayName')) {
        const random = new Uint32Array(2);
        crypto.getRandomValues(random);
        data.set('displayName', 'Garage');
        data.set('setupId', random[0].toString(36).toUpperCase().padStart(4, '0').slice(-4));
        data.set('setupCode', String(10000000 + (random[1] % 89999999)));
        data.set('relayGpio', '26');
        data.set('relayLevel', 'low');
        data.set('pulseMs', '1000');
        data.set('sensorGpio', '27');
        data.set('sensorLevel', 'low');
        data.set('sensorPull', 'up');
        data.set('feedbackEndpoint', 'closed');
        data.set('feedbackStabilityMs', '2000');
        data.set('openingSeconds', '20');
        data.set('closingSeconds', '20');
      }
      const relayGpio = String(data.get('relayGpio') || '');
      const sensorGpio = String(data.get('sensorGpio') || '');
      if (relayGpio === sensorGpio) {
        throw new Error(`Relay GPIO and feedback GPIO cannot both be GPIO ${relayGpio}. Choose two different pins.`);
      }
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

  async function saveInitialWifi(event) {
    saving = true;
    message = 'Saving first-time setup…';
    const data = new FormData(event.currentTarget);
    try {
      const random = new Uint32Array(2);
      crypto.getRandomValues(random);
      const body = new URLSearchParams({
        ssid: String(data.get('ssid') || ''),
        password: String(data.get('password') || ''),
        adminPassword: String(data.get('adminPassword') || ''),
        displayName: 'Garage',
        setupId: random[0].toString(36).toUpperCase().padStart(4, '0').slice(-4),
        setupCode: String(10000000 + (random[1] % 89999999)),
        relayGpio: '26', relayLevel: 'low', pulseMs: '1000',
        sensorGpio: '27', sensorLevel: 'low', sensorPull: 'up',
        feedbackEndpoint: 'closed', feedbackStabilityMs: '2000',
        openingSeconds: '20', closingSeconds: '20'
      });
      const response = await fetch('/save', {
        method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body
      });
      const text = await response.text();
      if (!response.ok) throw new Error(text.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim());
      message = 'Setup saved. The controller is restarting and connecting to your network.';
    } catch (error) {
      message = error instanceof Error ? error.message : 'Could not save Wi-Fi.';
      saving = false;
    }
  }

  async function downloadBackup(event) {
    saving = true; message = 'Creating an encrypted full backup…';
    const data = new FormData(event.currentTarget);
    try {
      const response = await fetch('/api/v1/backup', {
        method: 'POST',
        headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ password: String(data.get('password') || '') })
      });
      if (!response.ok) throw new Error(await response.text());
      const url = URL.createObjectURL(await response.blob());
      const anchor = document.createElement('a');
      anchor.href = url; anchor.download = 'garage-door-full.gdbak'; anchor.click();
      URL.revokeObjectURL(url); event.currentTarget.reset();
      message = 'Encrypted full backup downloaded.';
    } catch (error) { message = error instanceof Error ? error.message : 'Backup failed.'; }
    saving = false;
  }

  async function restoreBackup(event, file, setup = false) {
    if (!file) { message = 'Select a full backup file.'; return; }
    // Captive-portal web views can suppress modal confirm dialogs, making the
    // submit button appear inert. Fresh-setup confirmation is an explicit
    // required checkbox in SetupPage instead.
    if (!setup && !confirm('This will replace all device state and restart the controller. Continue?')) return;
    saving = true; message = 'Authenticating and staging the full backup…';
    const data = new FormData(event.currentTarget);
    try {
      const headers = { 'Content-Type': 'application/octet-stream', 'X-Backup-Password': String(data.get('password') || '') };
      if (!setup) headers['X-CSRF-Token'] = csrf;
      const response = await fetch(setup ? '/api/v1/setup/restore' : '/api/v1/restore', { method: 'POST', headers, body: file });
      if (!response.ok) throw new Error(await response.text());
      message = 'Backup validated and staged. The controller is restarting onto the restored network…';
    } catch (error) { message = error instanceof Error ? error.message : 'Restore failed.'; saving = false; }
  }

  async function downloadGateProfile() {
    saving = true; message = '';
    try {
      const response = await fetch('/api/v1/gate-profile', { cache: 'no-store' });
      if (!response.ok) throw new Error(await response.text());
      const url = URL.createObjectURL(await response.blob());
      const anchor = document.createElement('a');
      anchor.href = url; anchor.download = 'garage-door-gate-profile.json'; anchor.click();
      URL.revokeObjectURL(url);
      message = 'Complete non-secret Gate Profile downloaded.';
    } catch (error) { message = error instanceof Error ? error.message : 'Could not export Gate Profile.'; }
    saving = false;
  }

  async function reviewGateProfile(file) {
    if (!file) return;
    saving = true; message = 'Validating and normalizing the Gate Profile…'; gateProfileReview = null;
    try {
      const response = await fetch('/api/v1/gate-profile/preview', {
        method: 'POST', headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrf }, body: file
      });
      if (!response.ok) throw new Error(await response.text());
      gateProfileReview = await response.json();
      message = 'Profile passed strict validation. Review every replacement value below.';
    } catch (error) { message = error instanceof Error ? error.message : 'Could not import Gate Profile.'; }
    saving = false;
  }

  async function applyGateProfile() {
    if (!gateProfileReview) return;
    if (!confirm('Replace the complete Gate operator, wiring, timing, feedback, and decoder configuration with this reviewed profile? The controller will restart.')) return;
    saving = true; message = 'Applying the exact reviewed Gate Profile…';
    try {
      const response = await fetch('/api/v1/gate-profile/apply', {
        method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-CSRF-Token': csrf },
        body: new URLSearchParams({ digest: gateProfileReview.digest, confirmation: 'replace-gate-configuration' })
      });
      if (!response.ok) throw new Error(await response.text());
      gateProfileReview = null;
      message = 'Gate Profile applied atomically. The controller is restarting with the replacement configuration…';
    } catch (error) {
      message = error instanceof Error ? error.message : 'Could not apply Gate Profile.';
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

  async function resetHomeKitPairings() {
    if (!confirm('Reset all Apple Home pairings? This controller will be removed from every paired Apple Home and must be added again with the displayed pairing code. Wi-Fi and gate settings are preserved.')) return;
    saving = true;
    message = 'Resetting Apple Home pairings…';
    try {
      const response = await fetch('/api/v1/homekit/pairings', {
        method: 'DELETE', headers: { 'X-CSRF-Token': csrf }
      });
      if (!response.ok) throw new Error(await response.text());
      homekit = { ...homekit, paired: false, active: true };
      message = 'Apple Home pairings reset. Add the Garage Door Opener again using the pairing code below.';
    } catch (error) {
      message = error instanceof Error ? error.message : 'Could not reset Apple Home pairings.';
    }
    saving = false;
  }

  async function saveSettings(event) {
    saving = true; message = 'Validating settings…';
    const data = new FormData(event.currentTarget);
    const body = new URLSearchParams();
    for (const [key, value] of data) body.set(key, String(value));
    body.set('operatorProfile', operatorProfile);
    body.set('feedbackDecoder', 'customRules');
    serializeDecoder(body);
    try {
      const response = await fetch('/api/v1/config', {
        method: 'PUT', headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' }, body
      });
      if (!response.ok) throw new Error(await response.text());
      message = 'Settings saved. The controller is restarting to activate the GPIO and decoder configuration…';
      editing = false;
    } catch (error) { message = error instanceof Error ? error.message : 'Could not save settings.'; }
    saving = false;
  }

  function addDecoderInput() {
    if (decoder.inputs.length >= decoder.limits.inputs) return;
    const id = nextAssignedId(decoder.inputs);
    decoder.inputs.push({ id, label: `Input ${id}`, gpio: 27, activeLevel: 'high', pull: 'none', debounceMs: 30 });
  }

  function addDecoderRule() {
    if (!decoder.inputs.length || decoder.rules.length >= decoder.limits.rules) return;
    const id = nextAssignedId(decoder.rules);
    decoder.rules.push({ id, label: `Rule ${id}`, enabled: true, outcome: 'opened', entryConfirmationMs: 0,
      lossConfirmationMs: 0, matchAgeLimitMs: 0, matchAgeExpiry: 'none',
      groups: [[{ kind: 'stableLevel', inputId: decoder.inputs[0].id, level: 1, holdMs: 2000 }]] });
    editingPatternIndex = decoder.rules.length - 1;
  }

  function removeDecoderInput(input, index) {
    const references = inputReferenceCount(decoder.rules, input.id);
    if (references) {
      message = `${input.label} is used by ${references} rule predicate${references === 1 ? '' : 's'}. Remove those references first.`;
      return;
    }
    decoder.inputs.splice(index, 1);
  }

  const rulesFor = (outcomes) => decoder.rules.map((rule, index) => ({ rule, index })).filter(({ rule }) => outcomes.includes(rule.outcome));

  const inputName = (inputId) => decoder.inputs.find((input) => Number(input.id) === Number(inputId))?.label || 'Unknown input';

  function predicateSummary(predicate) {
    if (predicate.kind === 'periodicEdges') return `Periodic ${predicate.minimumIntervalMs}–${predicate.maximumIntervalMs} ms`;
    return `${inputName(predicate.inputId)} is ${Number(predicate.level) ? 'HIGH' : 'LOW'} for ${(Number(predicate.holdMs) / 1000).toFixed(1)} s`;
  }

  const groupSummary = (group) => group.map(predicateSummary).join(' AND ');

  function removePattern(index) {
    decoder.rules.splice(index, 1);
    editingPatternIndex = null;
  }

  function addGroup(rule) {
    if (rule.groups.length < decoder.limits.groupsPerRule) rule.groups.push([{ kind: 'stableLevel', inputId: decoder.inputs[0].id, level: 1, holdMs: 2000 }]);
  }

  function addPredicate(group) {
    if (group.length < decoder.limits.predicatesPerGroup) group.push({ kind: 'stableLevel', inputId: decoder.inputs[0].id, level: 1, holdMs: 2000 });
  }

  function removePredicate(rule, groupIndex, predicateIndex) {
    const group = rule.groups[groupIndex];
    if (group.length === 1) rule.groups.splice(groupIndex, 1);
    else group.splice(predicateIndex, 1);
  }

  const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

  async function learnPeriodic(predicate, key) {
    if (learningPredicate !== null) return;
    learningPredicate = key;
    learningProgress = 0;
    learningTrace = [];
    message = 'Learning… generate three transitions at the normal signal rate (15 second timeout).';
    const intervals = [];
    let previousLevel;
    let previousEdgeTimestamp;
    let lastSampleTimestamp;
    const startedAt = Date.now();
    try {
      while (Date.now() - startedAt < 15000) {
        const response = await fetch('/api/v1/runtime', { cache: 'no-store' });
        if (!response.ok) throw new Error(response.status === 401 ? 'Authentication expired.' : 'Could not read live decoder input.');
        const runtime = await response.json();
        const input = (runtime.decoderInputs || []).find((item) => Number(item.id) === Number(predicate.inputId));
        if (!runtime.decoderActive || !input) {
          throw new Error('This input is not active yet. Save the input configuration, let the controller restart, then reopen settings and learn the rule.');
        }
        const sampleTimestamp = Number(runtime.decoderSampleTimestampMs);
        if (previousLevel === undefined) {
          previousLevel = Boolean(input.level);
          lastSampleTimestamp = sampleTimestamp;
        } else if (sampleTimestamp !== lastSampleTimestamp) {
          lastSampleTimestamp = sampleTimestamp;
          const level = Boolean(input.level);
          if (level !== previousLevel) {
            if (previousEdgeTimestamp !== undefined) intervals.push(sampleTimestamp - previousEdgeTimestamp);
            previousEdgeTimestamp = sampleTimestamp;
            previousLevel = level;
          }
        }
        learningTrace = appendTrace(learningTrace, [{ id: String(predicate.inputId), level: Boolean(input.level) }], Date.now(), 15000);
        learningProgress = Math.min(100, Math.round((Date.now() - startedAt) / 150));
        const learned = consistentLearningIntervals(intervals);
        if (learned) {
          const margin = Math.max(100, Math.round(learned.median * 0.2), (learned.maximum - learned.minimum) * 2);
          predicate.minimumIntervalMs = Math.max(1, learned.minimum - margin);
          predicate.maximumIntervalMs = learned.maximum + margin;
          predicate.minimumEdges = 3;
          predicate.observationWindowMs = Math.ceil(predicate.maximumIntervalMs * 2 * 1.25);
          predicate.maximumGapMs = Math.ceil(predicate.maximumIntervalMs * 1.5);
          message = `Learned immediately from two consistent intervals (${learned.minimum}–${learned.maximum} ms). Review the safety margins, then save.`;
          return;
        }
        await wait(250);
      }
      throw new Error(`Learning timed out after 15 seconds. Captured ${intervals.length + (previousEdgeTimestamp === undefined ? 0 : 1)} transition(s); three transitions with two consistent intervals are required.`);
    } catch (error) {
      message = error instanceof Error ? error.message : 'Signal learning failed.';
    } finally {
      learningProgress = 0;
      learningTrace = [];
      learningPredicate = null;
    }
  }

  function serializeDecoder(body) {
    body.set('decoderInputCount', String(decoder.inputs.length));
    decoder.inputs.forEach((input, i) => {
      const p = `decoderInput${i}`;
      body.set(`${p}Id`, String(input.id)); body.set(`${p}Label`, input.label); body.set(`${p}Gpio`, String(input.gpio));
      body.set(`${p}Level`, input.activeLevel); body.set(`${p}Pull`, input.pull); body.set(`${p}DebounceMs`, String(input.debounceMs));
    });
    body.set('decoderRuleCount', String(decoder.rules.length));
    decoder.rules.forEach((rule, r) => {
      const p = `decoderRule${r}`;
      body.set(`${p}Id`, String(rule.id)); body.set(`${p}Label`, rule.label); body.set(`${p}Outcome`, rule.outcome);
      body.set(`${p}EntryMs`, String(rule.entryConfirmationMs || 0)); body.set(`${p}LossMs`, String(rule.lossConfirmationMs || 0));
      body.set(`${p}AgeMs`, String(rule.matchAgeLimitMs || 0)); body.set(`${p}Expiry`, rule.matchAgeExpiry || 'none');
      body.set(`${p}GroupCount`, String(rule.groups.length));
      rule.groups.forEach((group, g) => {
        const gp = `${p}Group${g}`; body.set(`${gp}PredicateCount`, String(group.length));
        group.forEach((predicate, x) => {
          const pp = `${gp}Predicate${x}`; body.set(`${pp}Kind`, predicate.kind); body.set(`${pp}InputId`, String(predicate.inputId));
          if (predicate.kind === 'stableLevel') {
            body.set(`${pp}Level`, String(predicate.level)); body.set(`${pp}HoldMs`, String(predicate.holdMs));
          } else {
            body.set(`${pp}MinimumIntervalMs`, String(predicate.minimumIntervalMs)); body.set(`${pp}MaximumIntervalMs`, String(predicate.maximumIntervalMs));
            body.set(`${pp}MinimumEdges`, String(predicate.minimumEdges)); body.set(`${pp}ObservationWindowMs`, String(predicate.observationWindowMs));
            body.set(`${pp}MaximumGapMs`, String(predicate.maximumGapMs));
          }
        });
      });
    });
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

  async function saveDisplayName(event) {
    event.preventDefault();
    const nextName = displayNameDraft.trim();
    if (!nextName || nextName.length > 64) {
      message = 'Display name must contain 1 to 64 characters.';
      return;
    }
    saving = true; message = 'Saving controller name…';
    try {
      const response = await fetch('/api/v1/config/name', {
        method: 'PUT',
        headers: { 'X-CSRF-Token': csrf, 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ displayName: nextName })
      });
      if (!response.ok) throw new Error(await response.text());
      config = { ...config, displayName: nextName };
      displayNameDraft = nextName;
      nameEditing = false;
      message = 'Controller name saved.';
    } catch (error) { message = error instanceof Error ? error.message : 'Could not save controller name.'; }
    saving = false;
  }

  async function controlGate(action) {
    const opening = action === 'open';
    if (!confirm(`${opening ? 'Open' : 'Close'} the gate now? Make sure the gate area is clear and can be observed safely.`)) return;
    pulsing = true;
    message = `Requesting gate ${action}…`;
    try {
      const response = await fetch(`/api/v1/gate/${action}`, {
        method: 'POST', headers: { 'X-CSRF-Token': csrf }
      });
      if (!response.ok) throw new Error(await response.text());
      message = `${opening ? 'Open' : 'Close'} command accepted.`;
    } catch (error) {
      message = error instanceof Error ? error.message : `Could not ${action} the gate.`;
    }
    pulsing = false;
  }

  loadStatus();
  setInterval(refreshRuntimeStatus, 250);
</script>

<svelte:head><meta name="description" content="Local Garage-Door-ESP32 configuration" /></svelte:head>

<AppHeader
  {status}
  {authenticated}
  {config}
  {saving}
  bind:nameEditing
  bind:displayNameDraft
  onSaveDisplayName={saveDisplayName}
  onSignOut={() => mutate('/api/v1/session/logout')}
/>

<main>
  {#if status.loading}
    <section class="card center"><div class="spinner"></div><p>Reading device status…</p></section>
  {:else if status.provisioned && !authenticated}
    <LoginPage {message} {saving} onLogin={login} />
  {:else if status.provisioned}
    <DashboardTabs {activeTab} onSelect={(tab) => { activeTab = tab; message = ''; }} />

    {#if activeTab === 'status'}
      <StatusPage {status} {config} />
    {:else if activeTab === 'access'}
      <AccessPage {homekit} {config} {saving} onChangePassword={changePassword} onResetHomeKitPairings={resetHomeKitPairings} />
    {:else if activeTab === 'network'}
      <NetworkPage {status} {config} {saving} onSave={saveWifiNetwork} />
    {:else if activeTab === 'gate'}
      <GatePage
        {config} {saving} {decoder} {pulsing} {liveInputs} {traceHistory} {traceNow} {gateProfileReview}
        {learningPredicate} {learningProgress} {learningTrace}
        bind:editing bind:operatorProfile bind:editingPatternIndex
        onToggleEditing={() => { editing = !editing; message = ''; }}
        onSaveSettings={saveSettings}
        onAddDecoderInput={addDecoderInput}
        onRemoveDecoderInput={removeDecoderInput}
        onAddDecoderRule={addDecoderRule}
        {rulesFor} {groupSummary} {inputName}
        onRemovePattern={removePattern}
        onAddGroup={addGroup}
        onAddPredicate={addPredicate}
        onRemovePredicate={removePredicate}
        onLearnPeriodic={learnPeriodic}
        onControlGate={controlGate}
        onExportProfile={downloadGateProfile}
        onImportProfile={reviewGateProfile}
        onApplyProfile={applyGateProfile}
        onCancelProfile={() => gateProfileReview = null}
      />
    {:else if activeTab === 'backup'}
      <BackupPage {saving} onBackup={downloadBackup} onRestore={(event, file) => restoreBackup(event, file, false)} />
    {:else if activeTab === 'firmware'}
      <FirmwarePage {firmware} bind:firmwareFile {otaUploading} {otaProgress} onUpload={uploadFirmware} onSelectFile={(file) => firmwareFile = file} />
    {:else}
      <LogsPage />
    {/if}
    {#if message}<p class="message">{message}</p>{/if}<button class="danger" onclick={() => mutate('/api/v1/system/reboot')}>Restart controller</button>
  {:else if !status.provisioned}
    <SetupPage {status} {message} {saving} onSave={saveInitialWifi} onRestore={(event, file) => restoreBackup(event, file, true)} />
  {/if}
</main>

<footer>Local configuration · No cloud connection · Relay disabled</footer>
