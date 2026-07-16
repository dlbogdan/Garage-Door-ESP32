import { describe, expect, it } from 'vitest';
import { appendTrace, commandStatus, consistentLearningIntervals, decodedStatus, inputReferenceCount, nextAssignedId, runtimePollMode, tracePath } from './gate-view.js';

describe('Gate overview helpers', () => {
  it('prefers custom decoder movement', () => {
    expect(decodedStatus({ feedbackDecoder: 'customRules', decoderActive: true, decoderMovement: 'opening', decoderPosition: 'closed' })).toBe('Opening');
  });

  it('maps directional and sequential commands', () => {
    expect(commandStatus({ activeCommand: 'OPEN' })).toBe('Open');
    expect(commandStatus({ activeCommand: 'STEP', target: 'CLOSED' })).toBe('Close');
    expect(commandStatus({ activeCommand: 'NONE' })).toBe('Idle');
    expect(commandStatus({ activeCommand: 'NONE', state: 'OPENING', target: 'OPEN' })).toBe('Open');
    expect(commandStatus({ activeCommand: 'NONE', state: 'CLOSING', target: 'CLOSED' })).toBe('Close');
    expect(commandStatus({ activeCommand: 'NONE', state: 'STOPPED_OPENING', target: 'CLOSED' })).toBe('Idle');
  });

  it('bounds history and resets when input identity changes', () => {
    let history = appendTrace([], [{ id: '1', level: false }], 1000);
    history = appendTrace(history, [{ id: '1', level: true }], 32000);
    expect(history).toHaveLength(1);
    history = appendTrace(history, [{ id: '2', level: true }], 32250);
    expect(history).toHaveLength(1);
    expect(tracePath(history, '2', 32250)).toContain('24');
  });

  it('polls Gate only while visible and leaves Status at one second', () => {
    expect(runtimePollMode({ authenticated: true, hidden: false, activeTab: 'gate', editing: false, elapsed: 0 })).toBe('gate');
    expect(runtimePollMode({ authenticated: true, hidden: true, activeTab: 'gate', editing: false, elapsed: 0 })).toBe('none');
    expect(runtimePollMode({ authenticated: true, hidden: false, activeTab: 'gate', editing: true, elapsed: 0 })).toBe('none');
    expect(runtimePollMode({ authenticated: true, hidden: false, activeTab: 'status', editing: false, elapsed: 1000 })).toBe('status');
  });

  it('assigns stable non-editable model IDs and counts references', () => {
    expect(nextAssignedId([{ id: 1 }, { id: 3 }])).toBe(2);
    const rules = [{ groups: [[{ inputId: 2 }, { inputId: 1 }], [{ inputId: 2 }]] }];
    expect(inputReferenceCount(rules, 2)).toBe(2);
    expect(inputReferenceCount(rules, 3)).toBe(0);
  });

  it('finishes learning after two consistent intervals', () => {
    expect(consistentLearningIntervals([1000])).toBeNull();
    expect(consistentLearningIntervals([1000, 1080])).toEqual({ minimum: 1000, maximum: 1080, median: 1040 });
    expect(consistentLearningIntervals([500, 1200])).toBeNull();
  });
});
