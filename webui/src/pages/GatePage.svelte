<script>
  import { commandStatus, decodedStatus, feedbackDecoderLabel, inputReferenceCount, operatorProfileLabel, tracePath } from '../gate-view.js';

  let {
    config, saving, decoder, pulsing, liveInputs, traceHistory, traceNow, learningPredicate, learningProgress, learningTrace,
    editing = $bindable(), operatorProfile = $bindable(), editingPatternIndex = $bindable(),
    onToggleEditing, onSaveSettings, onAddDecoderInput, onRemoveDecoderInput, onAddDecoderRule,
    rulesFor, groupSummary, inputName, onRemovePattern, onAddGroup, onAddPredicate, onRemovePredicate,
    onLearnPeriodic, onControlGate, onExportProfile, onImportProfile, gateProfileReview,
    onApplyProfile, onCancelProfile
  } = $props();
</script>

<section class="card">
  <div class="section-title"><span>GP</span><div><h3>Gate Profile</h3><p>Portable non-secret hardware, timing, feedback, and decoder configuration.</p></div></div>
  <div class="actions"><button type="button" disabled={saving} onclick={onExportProfile}>Download profile</button><label class="button secondary">Review imported profile<input class="visually-hidden" type="file" accept=".json,application/json" disabled={saving} onchange={(event) => onImportProfile(event.currentTarget.files?.[0] || null)} /></label></div>
  <p class="warning">Gate Profiles contain no Wi-Fi, administrator, HomeKit identity, pairing, or trace-history secrets. Import replaces the complete Gate wiring, timing, feedback, and decoder configuration; incorrect GPIO assignments can energize unintended controller inputs.</p>
  {#if gateProfileReview}
    <section class="profile-review" aria-labelledby="profile-review-title">
      <h4 id="profile-review-title">Validated replacement review</h4>
      <p><strong>{gateProfileReview.target.name || 'Unnamed profile'}</strong> · {gateProfileReview.target.vendor || 'Unspecified vendor'} {gateProfileReview.target.model || 'Unspecified model'}</p>
      {#if gateProfileReview.target.notes}<p>{gateProfileReview.target.notes}</p>{/if}
      <div class="grid">
        <article><small>Operator</small><strong>{gateProfileReview.current.operator.profile} → {gateProfileReview.candidate.operator.profile}</strong></article>
        <article><small>Feedback</small><strong>{gateProfileReview.current.operator.feedback.topology} → {gateProfileReview.candidate.operator.feedback.topology}</strong></article>
        <article><small>Decoder</small><strong>{gateProfileReview.current.operator.feedback.decoder.profile} → {gateProfileReview.candidate.operator.feedback.decoder.profile}</strong></article>
        <article><small>Decoder size</small><strong>{gateProfileReview.current.operator.feedback.decoder.inputs.length}/{gateProfileReview.current.operator.feedback.decoder.rules.length} → {gateProfileReview.summary.decoderInputs}/{gateProfileReview.summary.decoderRules}</strong></article>
        <article><small>Travel timing</small><strong>{gateProfileReview.current.timing.openingMs}/{gateProfileReview.current.timing.closingMs} ms → {gateProfileReview.summary.openingMs}/{gateProfileReview.summary.closingMs} ms</strong></article>
        <article><small>Release timeout</small><strong>{gateProfileReview.current.timing.sensorReleaseTimeoutMs} ms → {gateProfileReview.summary.sensorReleaseTimeoutMs} ms</strong></article>
      </div>
      <details><summary>Review complete normalized JSON</summary><div class="profile-json"><div><h5>Current Gate configuration</h5><pre>{JSON.stringify(gateProfileReview.current, null, 2)}</pre></div><div><h5>Imported replacement</h5><pre>{JSON.stringify(gateProfileReview.candidate, null, 2)}</pre></div></div></details>
      <p class="digest">Reviewed digest: <code>{gateProfileReview.digest}</code></p>
      <div class="actions"><button type="button" class="primary" disabled={saving} onclick={onApplyProfile}>Confirm complete replacement</button><button type="button" class="secondary" disabled={saving} onclick={onCancelProfile}>Cancel review</button></div>
    </section>
  {/if}
</section>

<section class="card"><div class="section-title"><span>IO</span><div><h3>Gate hardware & timing</h3><p>Relay, feedback sensor, pulse logic, and travel timeouts.</p></div><button class="secondary edit" onclick={onToggleEditing}>{editing ? 'Cancel' : 'Edit settings'}</button></div>
        {#if editing}
          <div class="profile-picker"><label>Operator profile<select bind:value={operatorProfile}><option value="sequential">Step by step</option><option value="directional">Directional · separate OPEN / CLOSE</option></select></label><p>{operatorProfile === 'sequential' ? 'One ESP output pulses the gate operator control input step by step.' : 'Separate ESP outputs pulse the gate operator OPEN and CLOSE inputs.'}</p></div>
          <form onsubmit={(event) => { event.preventDefault(); onSaveSettings(event); }}>
            <input type="hidden" name="operatorProfile" value={operatorProfile} />
            <input type="hidden" name="displayName" value={config?.displayName} />
            <input type="hidden" name="feedbackStabilityMs" value={config?.feedbackStabilityMs || 2000} />
            <div class="command-settings">
              {#if operatorProfile === 'sequential'}
                <fieldset class="command-group"><legend>ESP Output</legend><div class="grid"><label>GPIO<input name="stepGpio" type="number" min="0" max="39" value={config?.relayGpio} required /></label><label>Active level<select name="stepLevel" value={config?.relayActiveHigh ? 'high' : 'low'}><option value="low">Low</option><option value="high">High</option></select></label><label>Pulse (ms)<input name="stepPulseMs" type="number" min="100" max="2000" value={config?.pulseMs} required /></label><label>Opening timeout (s)<input name="openingSeconds" type="number" min="3" max="180" value={config?.openingSeconds} required /></label><label>Closing timeout (s)<input name="closingSeconds" type="number" min="3" max="180" value={config?.closingSeconds} required /></label></div></fieldset>
              {:else}
                <fieldset class="command-group"><legend>Open ESP Output</legend><div class="grid"><label>GPIO<input name="openGpio" type="number" min="0" max="39" value={config?.openGpio} required /></label><label>Active level<select name="openLevel" value={config?.openActiveHigh ? 'high' : 'low'}><option value="low">Low</option><option value="high">High</option></select></label><label>Pulse (ms)<input name="openPulseMs" type="number" min="100" max="2000" value={config?.openPulseMs || 500} required /></label><label>Opening timeout (s)<input name="openingSeconds" type="number" min="3" max="180" value={config?.openingSeconds} required /></label></div></fieldset>
                <fieldset class="command-group"><legend>Close ESP Output</legend><div class="grid"><label>GPIO<input name="closeGpio" type="number" min="0" max="39" value={config?.closeGpio} required /></label><label>Active level<select name="closeLevel" value={config?.closeActiveHigh ? 'high' : 'low'}><option value="low">Low</option><option value="high">High</option></select></label><label>Pulse (ms)<input name="closePulseMs" type="number" min="100" max="2000" value={config?.closePulseMs || 500} required /></label><label>Closing timeout (s)<input name="closingSeconds" type="number" min="3" max="180" value={config?.closingSeconds} required /></label></div></fieldset>
              {/if}
            </div>
            <section class="feedback-section"><div class="feedback-heading"><div><p class="eyebrow">Feedback</p><h4>Custom Signal Rules</h4><p>Declare electrical inputs, then map observed signals to gate state.</p></div></div>
              <div class="feedback-workspace"><fieldset class="input-column"><legend>Inputs</legend>
                {#if !decoder.inputs.length}<p class="empty-copy">No feedback inputs. Add one to begin explicit commissioning.</p>{/if}
                {#each decoder.inputs as input, i}<article class="config-item"><button type="button" class="context-remove" disabled={inputReferenceCount(decoder.rules, input.id) > 0} aria-label={`Remove ${input.label}`} title={inputReferenceCount(decoder.rules, input.id) ? 'Referenced by a rule' : 'Remove input'} onclick={() => onRemoveDecoderInput(input, i)}>×</button><div class="item-title"><strong>{input.label}</strong><span>ID {input.id}</span></div><div class="grid compact"><label>Label<input maxlength="32" bind:value={input.label} required /></label><label>GPIO<input type="number" min="0" max="39" bind:value={input.gpio} required /></label><label>Active level<select bind:value={input.activeLevel}><option value="high">High</option><option value="low">Low</option></select></label><label>Pull<select bind:value={input.pull}><option value="none">None</option><option value="up">Pull-up</option><option value="down">Pull-down</option></select></label><label>Debounce (ms)<input type="number" min="10" max="500" bind:value={input.debounceMs} required /></label></div></article>{/each}
                <button type="button" class="context-add" aria-label="Add input" title="Add input" disabled={decoder.inputs.length >= decoder.limits.inputs} onclick={onAddDecoderInput}>+</button>
              </fieldset><section class="patterns-column" aria-labelledby="signal-patterns-title"><div class="patterns-heading"><div><h5 id="signal-patterns-title">Signal Patterns</h5><p>When an electrical pattern is observed, interpret it as a gate state.</p></div></div>
                {#if !decoder.rules.length}<p class="empty-copy pattern-empty">No signal patterns configured. Gate feedback remains unknown.</p>{/if}
                {#each [['Position',['opened','closed']],['Motion',['opening','closing','stopped']],['Faults',['obstructed']]] as category}<section class="pattern-category"><h5>{category[0]}</h5>
                  {#each rulesFor(category[1]) as entry}{@const rule = entry.rule}{@const r = entry.index}
                    <article class="pattern-item" class:editing-pattern={editingPatternIndex === r}>
                      <div class="pattern-summary"><div class="pattern-description">{#each rule.groups as group, g}<strong>{groupSummary(group)}</strong>{#if g < rule.groups.length - 1}<span class="summary-or">OR</span>{/if}{/each}<small>{[...new Set(rule.groups.flat().map((predicate) => inputName(predicate.inputId)))].join(', ')}</small></div><strong class="pattern-outcome">{rule.outcome.toUpperCase()}</strong><button type="button" class="secondary pattern-edit" onclick={() => editingPatternIndex = editingPatternIndex === r ? null : r}>{editingPatternIndex === r ? 'Close' : 'Edit'}</button></div>
                      {#if editingPatternIndex === r}<div class="pattern-editor"><div class="pattern-editor-heading"><div><p class="eyebrow">Interpret as</p><select aria-label="Interpreted gate state" bind:value={rule.outcome} onchange={() => rule.label = rule.outcome.toUpperCase()}><option value="opened">OPENED</option><option value="closed">CLOSED</option><option value="opening">OPENING</option><option value="closing">CLOSING</option><option value="stopped">STOPPED</option><option value="obstructed">OBSTRUCTED</option></select></div><button type="button" class="delete-pattern" onclick={() => onRemovePattern(r)}>Delete pattern</button></div>
                        <section class="signal-editor"><h6>Signal pattern</h6>{#each rule.groups as group, g}{#if g > 0}<div class="or-divider"><span>OR</span></div>{/if}<div class="signal-option">{#each group as predicate, p}{#if p > 0}<div class="and-divider"><span>AND</span></div>{/if}<div class="signal-condition"><button type="button" class="predicate-remove" aria-label="Remove condition" title="Remove condition" disabled={learningPredicate === `${r}-${g}-${p}`} onclick={() => onRemovePredicate(rule, g, p)}>×</button><div class="grid"><label>Signal type<select bind:value={predicate.kind} onchange={() => { if (predicate.kind === 'stableLevel') Object.assign(predicate, { level: 1, holdMs: 2000 }); else Object.assign(predicate, { minimumIntervalMs: 800, maximumIntervalMs: 1200, minimumEdges: 3, observationWindowMs: 3500, maximumGapMs: 1500 }); }}><option value="stableLevel">Stable level</option><option value="periodicEdges">Periodic toggling</option></select></label><label>Input<select bind:value={predicate.inputId}>{#each decoder.inputs as input}<option value={input.id}>{input.label}</option>{/each}</select></label>
                            {#if predicate.kind === 'stableLevel'}<label>Logical level<select bind:value={predicate.level}><option value={1}>HIGH</option><option value={0}>LOW</option></select></label><label>Continuous hold (ms)<input type="number" min="1" bind:value={predicate.holdMs} required /></label>{:else}<label>Minimum interval (ms)<input type="number" min="1" bind:value={predicate.minimumIntervalMs} required /></label><label>Maximum interval (ms)<input type="number" min={predicate.minimumIntervalMs} bind:value={predicate.maximumIntervalMs} required /></label><label>Minimum edges<input type="number" min="2" max="16" bind:value={predicate.minimumEdges} required /></label><label>Observation window (ms)<input type="number" min={(predicate.minimumEdges - 1) * predicate.minimumIntervalMs} bind:value={predicate.observationWindowMs} required /></label><label>Maximum gap (ms)<input type="number" min={predicate.maximumIntervalMs} bind:value={predicate.maximumGapMs} required /></label><div class="learn-action"><button type="button" class="secondary" disabled={learningPredicate !== null} onclick={() => onLearnPeriodic(predicate, `${r}-${g}-${p}`)}>{learningPredicate === `${r}-${g}-${p}` ? `Learning… ${learningProgress}%` : 'Learn signal'}</button></div>{#if learningPredicate === `${r}-${g}-${p}`}<svg class="mini-scope" viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Selected input learning trace"><path class="trace-grid" d="M0 50H100 M25 0V100 M50 0V100 M75 0V100"/><path class="trace-line" d={tracePath(learningTrace, String(predicate.inputId), Date.now(), 15000)}/></svg>{/if}{/if}</div></div>{/each}<button type="button" class="text-add" disabled={group.length >= decoder.limits.predicatesPerGroup || learningPredicate !== null} onclick={() => onAddPredicate(group)}>+ Add AND condition</button></div>{/each}<button type="button" class="text-add add-or" disabled={rule.groups.length >= decoder.limits.groupsPerRule} onclick={() => onAddGroup(rule)}>+ Add OR pattern</button></section>
                        <section class="behaviour-editor"><h6>Rule behaviour</h6><div class="grid"><label>Entry confirmation (ms)<input type="number" min="0" bind:value={rule.entryConfirmationMs} required /></label><label>Loss confirmation (ms)<input type="number" min="0" bind:value={rule.lossConfirmationMs} required /></label><label>Match-age limit (ms)<input type="number" min="0" bind:value={rule.matchAgeLimitMs} required /></label><label>After match-age limit<select bind:value={rule.matchAgeExpiry}><option value="none">No expiry</option><option value="unknown">UNKNOWN</option><option value="obstructed">OBSTRUCTED</option><option value="unknownAndObstructed">UNKNOWN + OBSTRUCTED</option></select></label></div></section>
                        <button type="button" class="secondary close-pattern" onclick={() => editingPatternIndex = null}>Done</button></div>{/if}
                    </article>
                  {/each}</section>{/each}
                <button type="button" class="secondary add-pattern" disabled={!decoder.inputs.length || decoder.rules.length >= decoder.limits.rules} onclick={onAddDecoderRule}>+ Add Signal Pattern</button>
              </section></div>
            </section>
            <button class="primary save-gate" disabled={saving}>{saving ? 'Saving…' : 'Save Gate Settings'}<span>→</span></button>
          </form>
        {:else}
          <div class="gate-overview">
            <div class="gate-summary" aria-label="Gate configuration summary">
              <article><span class="text-mark">OP</span><div><small>Operator profile</small><strong>{operatorProfileLabel(config?.operatorProfile)}</strong></div></article>
              <article><span class="text-mark decoder-mark">FD</span><div><small>Feedback decoder</small><strong>{feedbackDecoderLabel(config)}</strong></div></article>
            </div>
            <section class="live-panel" aria-labelledby="live-signal-title">
              <div class="live-heading"><div><p class="eyebrow">Live input monitor</p><h4 id="live-signal-title">Feedback signals</h4><p>Coarse 250 ms browser view · last 30 seconds</p></div><span class="live-badge"><i></i> Live</span></div>
              {#if liveInputs.length}
                <div class="logic-analyzer">
                  {#each liveInputs as input}
                    <div class="trace-row">
                      <div class="trace-label"><span class:high={input.level}></span><strong>{input.label}</strong><small>{input.level ? 'HIGH' : 'LOW'}</small></div>
                      <svg viewBox="0 0 100 100" preserveAspectRatio="none" role="img" aria-label={`${input.label}: ${input.level ? 'high' : 'low'}`}>
                        <path class="trace-grid" d="M0 25H100 M0 50H100 M0 75H100 M25 0V100 M50 0V100 M75 0V100" />
                        <path class="trace-line" d={tracePath(traceHistory, input.id, traceNow)} />
                      </svg>
                    </div>
                  {/each}
                  <div class="trace-time"><span>−30 s</span><span>−15 s</span><span>Now</span></div>
                </div>
              {:else}<p class="trace-empty">No feedback inputs are currently available.</p>{/if}
            </section>
            <div class="runtime-summary" aria-live="polite">
              <article class:unknown={decodedStatus(config) === 'Unknown'}><small>Decoded status</small><strong>{decodedStatus(config)}</strong><span class:alert={config?.decoderObstructed || config?.obstruction}>{config?.decoderObstructed || config?.obstruction ? '+ Obstructed' : 'No obstruction'}</span></article>
              <article><small>Command</small><strong>{commandStatus(config)}</strong><span>{config?.pulseActive ? 'Output pulse active' : 'Controller idle'}</span></article>
            </div>
            {#if config?.decoderActive}<details class="decoder-details"><summary>Decoder evidence and rule diagnostics</summary><dl class="settings"><div><dt>Health</dt><dd>{config?.decoderHealth}</dd></div>{#each config?.decoderPredicates || [] as predicate}<div><dt>Predicate {predicate.index + 1}</dt><dd>{predicate.value ? 'true' : 'false'} · edge {predicate.latestIntervalMs} ms · {predicate.qualifyingEdgeCount} qualifying edges</dd></div>{/each}{#each config?.decoderRules || [] as rule}<div><dt>Rule {rule.id}</dt><dd>{rule.expressionValue ? 'matching' : 'not matching'} · {rule.phase} · {rule.matchAgeMs} ms</dd></div>{/each}</dl></details>{/if}
            <div class="gate-action"><div><strong>Local gate control</strong><p>Fallback control when Apple Home is unavailable. Keep the gate area in view; normal operator strategy and safety interlocks apply.</p></div><div class="gate-buttons"><button class="primary close-gate" disabled={pulsing || !config?.relayControlEnabled} onclick={() => onControlGate('close')}>{pulsing ? 'Sending…' : 'Close gate'}<span>↓</span></button><button class="primary" disabled={pulsing || !config?.relayControlEnabled} onclick={() => onControlGate('open')}>{pulsing ? 'Sending…' : 'Open gate'}<span>↑</span></button></div></div>
          </div>
        {/if}
      </section>
