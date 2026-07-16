export const TRACE_WINDOW_MS = 30000;

const title = (value) => value ? value[0].toUpperCase() + value.slice(1).toLowerCase() : 'Unknown';

export function operatorProfileLabel(profile) {
  return profile === 'directional' ? 'Separate OPEN / CLOSE inputs' : 'One STEP input';
}

export function feedbackDecoderLabel(config) {
  if (config?.feedbackDecoder === 'customRules') {
    const inputs = Number(config.decoderInputCount ?? config.decoder?.inputs?.length ?? 0);
    const rules = Number(config.decoderRuleCount ?? config.decoder?.rules?.length ?? 0);
    return `Custom signal rules · ${inputs} input${inputs === 1 ? '' : 's'} · ${rules} rule${rules === 1 ? '' : 's'}`;
  }
  return config?.feedbackMode === 'dual' ? 'Endpoint preset · dual input' : 'Endpoint preset · single input';
}

export function decodedStatus(config) {
  if (config?.feedbackDecoder === 'customRules' && config.decoderActive) {
    const movement = String(config.decoderMovement || 'unknown').toLowerCase();
    if (movement !== 'unknown') return title(movement);
    return title(String(config.decoderPosition || 'unknown'));
  }
  const state = String(config?.state || config?.observation || 'unknown').toLowerCase();
  return ['open', 'opened'].includes(state) ? 'Open'
    : state === 'closed' ? 'Closed'
    : ['opening', 'closing', 'stopped'].includes(state) ? title(state)
    : 'Unknown';
}

export function commandStatus(config) {
  const command = String(config?.activeCommand || 'NONE').toUpperCase();
  if (command === 'OPEN') return 'Open';
  if (command === 'CLOSE') return 'Close';
  if (command === 'STEP') {
    const target = String(config?.target || '').toUpperCase();
    if (target.includes('OPEN')) return 'Open';
    if (target.includes('CLOSE')) return 'Close';
  }
  // activeCommand describes only the short electrical pulse and returns to
  // NONE while the gate continues travelling. Preserve the requested command
  // in the UI for the full controller-tracked movement instead.
  const state = String(config?.state || '').toUpperCase();
  if (state === 'OPENING') return 'Open';
  if (state === 'CLOSING') return 'Close';
  return 'Idle';
}

export function runtimePollMode({ authenticated, hidden, activeTab, editing, elapsed }) {
  if (!authenticated || hidden) return 'none';
  if (activeTab === 'gate' && !editing) return 'gate';
  if (activeTab === 'status' && elapsed >= 1000) return 'status';
  return 'none';
}

export function traceInputs(config, decoder) {
  if (config?.feedbackDecoder === 'customRules') {
    const labels = new Map((decoder?.inputs || []).map((input) => [Number(input.id), input.label]));
    return (config?.decoderInputs || []).map((input) => ({ id: String(input.id), label: labels.get(Number(input.id)) || `Input ${input.id}`, level: Boolean(input.level) }));
  }
  if (config?.feedbackMode === 'dual') return [
    { id: 'opened', label: 'OPENED input', level: Boolean(config?.openedAsserted) },
    { id: 'closed', label: 'CLOSED input', level: Boolean(config?.closedAsserted) }
  ];
  const endpoint = config?.feedbackActiveEndpoint === 'open' ? 'OPEN' : 'CLOSED';
  return [{ id: 'feedback', label: `${endpoint} input`, level: endpoint === 'OPEN' ? Boolean(config?.openedAsserted) : Boolean(config?.closedAsserted) }];
}

export function appendTrace(history, inputs, now, windowMs = TRACE_WINDOW_MS) {
  const signature = inputs.map((input) => input.id).join('|');
  const previous = history.length ? Object.keys(history[history.length - 1].values).join('|') : signature;
  const base = signature === previous ? history : [];
  const values = Object.fromEntries(inputs.map((input) => [input.id, input.level]));
  return [...base.filter((sample) => sample.time >= now - windowMs), { time: now, values }];
}

export function tracePath(history, inputId, now, windowMs = TRACE_WINDOW_MS) {
  if (!history.length) return '';
  const points = history.map((sample) => ({ x: Math.max(0, Math.min(100, 100 - ((now - sample.time) / windowMs) * 100)), y: sample.values[inputId] ? 24 : 76 }));
  let path = `M 0 ${points[0].y} L ${points[0].x.toFixed(2)} ${points[0].y}`;
  for (let index = 1; index < points.length; index += 1) path += ` H ${points[index].x.toFixed(2)} V ${points[index].y}`;
  return `${path} H 100`;
}
