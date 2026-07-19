export const STATE_VERSION = 1;
export const CALIBRATION_NAME = "cloud-offset-median-v1";

export const DEFAULT_BATTERY_CONFIG = Object.freeze({
  offsetMv: 0,
  emptyMv: 3300,
  fullMv: 4200,
  maxMv: 4220,
  sampleCount: 5,
  displayStepMv: 10,
  lowPct: 20,
  criticalPct: 10,
  lowRecoveryPct: 25,
  criticalRecoveryPct: 15,
  repeatAlertSecs: 86400,
});

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function finiteInteger(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? Math.round(number) : fallback;
}

export function calibrateVoltage(rawMv, config = DEFAULT_BATTERY_CONFIG) {
  const value = finiteInteger(rawMv, -1);
  if (value <= 0) return 0;
  return clamp(value + config.offsetMv, 0, config.maxMv);
}

export function linearPercent(mv, config = DEFAULT_BATTERY_CONFIG) {
  if (!Number.isFinite(Number(mv)) || Number(mv) <= 0) return -1;
  const span = config.fullMv - config.emptyMv;
  if (span <= 0) return -1;
  return clamp(Math.round(((Number(mv) - config.emptyMv) * 100) / span), 0, 100);
}

export function median(values) {
  const sorted = values.filter(Number.isFinite).slice().sort((a, b) => a - b);
  if (sorted.length === 0) return 0;
  const middle = Math.floor(sorted.length / 2);
  if (sorted.length % 2 === 1) return sorted[middle];
  return Math.round((sorted[middle - 1] + sorted[middle]) / 2);
}

export function roundToStep(value, step) {
  if (!Number.isFinite(value) || !Number.isFinite(step) || step <= 0) return value;
  return Math.round(value / step) * step;
}

export function normalizeState(input) {
  const source = input && typeof input === "object" ? input : {};
  const samples = Array.isArray(source.samples)
    ? source.samples
        .filter((sample) => sample && Number.isFinite(Number(sample.ts)) && Number.isFinite(Number(sample.corrected_mv)))
        .map((sample) => ({
          ts: finiteInteger(sample.ts),
          raw_mv: finiteInteger(sample.raw_mv),
          corrected_mv: finiteInteger(sample.corrected_mv),
        }))
    : [];
  return {
    version: STATE_VERSION,
    last_raw_ts: finiteInteger(source.last_raw_ts),
    samples,
    level: ["ok", "low", "critical"].includes(source.level) ? source.level : "ok",
    last_alert_at: finiteInteger(source.last_alert_at),
    last_alert_level: ["low", "critical"].includes(source.last_alert_level) ? source.last_alert_level : null,
  };
}

function alertLevelForPercent(percent, previousLevel, config) {
  if (previousLevel === "critical" && percent <= config.criticalRecoveryPct) return "critical";
  if (percent <= config.criticalPct) return "critical";
  if (previousLevel === "low" && percent <= config.lowRecoveryPct) return "low";
  if (percent <= config.lowPct) return "low";
  return "ok";
}

function shouldAlert(level, state, nowSec, config) {
  if (level === "ok") return false;
  const escalation = level === "critical" && state.last_alert_level !== "critical";
  const firstLow = level === "low" && state.last_alert_level == null;
  const repeatDue = state.last_alert_at <= 0 || nowSec - state.last_alert_at >= config.repeatAlertSecs;
  return escalation || firstLow || repeatDue;
}

export function processTelemetry({ heartbeat, status, state, nowSec, config = DEFAULT_BATTERY_CONFIG }) {
  if (!heartbeat || typeof heartbeat !== "object") {
    return { changed: false, reason: "missing-heartbeat" };
  }
  if (heartbeat.calibrated === true || heartbeat.calibration === CALIBRATION_NAME) {
    return { changed: false, reason: "already-calibrated" };
  }

  const rawMv = finiteInteger(heartbeat.mv, -1);
  const heartbeatTs = finiteInteger(heartbeat.ts, 0);
  if (rawMv <= 0 || heartbeatTs <= 0) {
    return { changed: false, reason: "invalid-heartbeat" };
  }

  const nextState = normalizeState(state);
  if (nextState.last_raw_ts >= heartbeatTs) {
    return { changed: false, reason: "already-processed" };
  }

  const correctedSampleMv = calibrateVoltage(rawMv, config);
  nextState.samples.push({ ts: heartbeatTs, raw_mv: rawMv, corrected_mv: correctedSampleMv });
  nextState.samples = nextState.samples
    .sort((a, b) => a.ts - b.ts)
    .slice(-config.sampleCount);
  nextState.last_raw_ts = heartbeatTs;

  const filteredMv = median(nextState.samples.map((sample) => sample.corrected_mv));
  const displayMv = clamp(roundToStep(filteredMv, config.displayStepMv), 0, config.maxMv);
  const percent = linearPercent(displayMv, config);
  const previousLevel = nextState.level;
  const level = alertLevelForPercent(percent, previousLevel, config);
  const alertDue = shouldAlert(level, nextState, nowSec, config);
  nextState.level = level;

  const low = level !== "ok";
  const calibratedHeartbeat = {
    ...heartbeat,
    raw_mv: rawMv,
    raw_pct: finiteInteger(heartbeat.pct, -1),
    mv: displayMv,
    pct: percent,
    low,
    battery_alert: level,
    calibrated: true,
    calibration: CALIBRATION_NAME,
    calibration_offset_mv: config.offsetMv,
    filtered_samples: nextState.samples.length,
  };

  const sourceStatus = status && typeof status === "object" ? status : {};
  const calibratedStatus = {
    ...sourceStatus,
    battery_raw_mv: finiteInteger(sourceStatus.battery_mv, rawMv),
    battery_raw_pct: finiteInteger(sourceStatus.battery_pct, finiteInteger(heartbeat.pct, -1)),
    battery_mv: displayMv,
    battery_pct: percent,
    battery_low: low,
    battery_alert: level,
    battery_calibrated: true,
    battery_calibration: CALIBRATION_NAME,
    battery_calibration_offset_mv: config.offsetMv,
  };

  return {
    changed: true,
    rawMv,
    correctedSampleMv,
    filteredMv,
    displayMv,
    percent,
    level,
    previousLevel,
    alertDue,
    heartbeat: calibratedHeartbeat,
    status: calibratedStatus,
    state: nextState,
  };
}

export function markAlertSent(state, level, nowSec) {
  const nextState = normalizeState(state);
  nextState.last_alert_at = finiteInteger(nowSec);
  nextState.last_alert_level = level === "critical" ? "critical" : "low";
  return nextState;
}
