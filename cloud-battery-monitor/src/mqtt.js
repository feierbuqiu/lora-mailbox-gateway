const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

function concatBytes(...parts) {
  const size = parts.reduce((total, part) => total + part.length, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.length;
  }
  return output;
}

function mqttString(value) {
  const bytes = textEncoder.encode(String(value));
  if (bytes.length > 0xffff) throw new Error("MQTT string is too long");
  return concatBytes(new Uint8Array([bytes.length >> 8, bytes.length & 0xff]), bytes);
}

function remainingLength(value) {
  const output = [];
  let remaining = value;
  do {
    let digit = remaining % 128;
    remaining = Math.floor(remaining / 128);
    if (remaining > 0) digit |= 0x80;
    output.push(digit);
  } while (remaining > 0);
  return new Uint8Array(output);
}

function frame(header, body) {
  return concatBytes(new Uint8Array([header]), remainingLength(body.length), body);
}

export function connectPacket(clientId, username, password) {
  const flags = 0x02 | (username ? 0x80 : 0) | (password ? 0x40 : 0);
  const variableHeader = concatBytes(mqttString("MQTT"), new Uint8Array([4, flags, 0, 20]));
  const payload = concatBytes(
    mqttString(clientId),
    username ? mqttString(username) : new Uint8Array(),
    password ? mqttString(password) : new Uint8Array(),
  );
  return frame(0x10, concatBytes(variableHeader, payload));
}

export function subscribePacket(packetId, topics) {
  const topicBytes = topics.map((topic) => concatBytes(mqttString(topic), new Uint8Array([0])));
  const body = concatBytes(new Uint8Array([packetId >> 8, packetId & 0xff]), ...topicBytes);
  return frame(0x82, body);
}

export function publishPacket(topic, payload, { retain = true } = {}) {
  const body = concatBytes(mqttString(topic), textEncoder.encode(payload));
  return frame(0x30 | (retain ? 0x01 : 0), body);
}

function appendBuffer(current, incoming) {
  return concatBytes(current, incoming);
}

function decodeFrames(buffer) {
  const packets = [];
  let position = 0;
  while (position < buffer.length) {
    const start = position;
    if (buffer.length - position < 2) break;
    const header = buffer[position++];
    let multiplier = 1;
    let length = 0;
    let digit;
    let count = 0;
    do {
      if (position >= buffer.length) return { packets, rest: buffer.slice(start) };
      digit = buffer[position++];
      length += (digit & 0x7f) * multiplier;
      multiplier *= 128;
      count += 1;
      if (count > 4) throw new Error("Invalid MQTT remaining length");
    } while (digit & 0x80);
    if (buffer.length - position < length) return { packets, rest: buffer.slice(start) };
    packets.push({ header, body: buffer.slice(position, position + length) });
    position += length;
  }
  return { packets, rest: buffer.slice(position) };
}

function parsePublish(packet) {
  if ((packet.header >> 4) !== 3 || packet.body.length < 2) return null;
  const topicLength = (packet.body[0] << 8) | packet.body[1];
  if (packet.body.length < 2 + topicLength) return null;
  const topic = textDecoder.decode(packet.body.slice(2, 2 + topicLength));
  const qos = (packet.header >> 1) & 0x03;
  const payloadOffset = 2 + topicLength + (qos > 0 ? 2 : 0);
  return {
    topic,
    payload: textDecoder.decode(packet.body.slice(payloadOffset)),
    retained: Boolean(packet.header & 0x01),
  };
}

async function eventBytes(data) {
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  if (typeof Blob !== "undefined" && data instanceof Blob) return new Uint8Array(await data.arrayBuffer());
  if (typeof data === "string") return textEncoder.encode(data);
  throw new Error("Unsupported WebSocket MQTT frame type");
}

function openMqttSocket({ url, username, password, clientId, timeoutMs = 7000, WebSocketImpl = WebSocket }) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocketImpl(url, "mqtt");
    socket.binaryType = "arraybuffer";
    let buffer = new Uint8Array();
    let settled = false;
    const timer = setTimeout(() => finish(new Error("MQTT connection timed out")), timeoutMs);

    function finish(error) {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (error) {
        try { socket.close(); } catch {}
        reject(error);
      } else {
        resolve(socket);
      }
    }

    socket.addEventListener("open", () => {
      socket.send(connectPacket(clientId, username, password));
    });
    socket.addEventListener("error", () => finish(new Error("MQTT WebSocket connection failed")));
    socket.addEventListener("close", () => {
      if (!settled) finish(new Error("MQTT WebSocket closed before CONNACK"));
    });
    socket.addEventListener("message", async (event) => {
      try {
        buffer = appendBuffer(buffer, await eventBytes(event.data));
        const decoded = decodeFrames(buffer);
        buffer = decoded.rest;
        for (const packet of decoded.packets) {
          if ((packet.header >> 4) === 2) {
            if (packet.body.length !== 2 || packet.body[1] !== 0) {
              finish(new Error(`MQTT broker rejected connection (${packet.body[1] ?? "unknown"})`));
              return;
            }
            finish();
            return;
          }
        }
      } catch (error) {
        finish(error);
      }
    });
  });
}

export async function readRetainedTopics(options) {
  const {
    topics,
    requiredTopic = topics[0],
    settleMs = 250,
    timeoutMs = 7000,
  } = options;
  const socket = await openMqttSocket(options);
  const messages = {};
  let buffer = new Uint8Array();

  return new Promise((resolve, reject) => {
    let settled = false;
    let settleTimer = null;
    const timer = setTimeout(() => finish(new Error(`Timed out waiting for retained topic ${requiredTopic}`)), timeoutMs);

    function finish(error) {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (settleTimer) clearTimeout(settleTimer);
      try { socket.close(1000, "snapshot complete"); } catch {}
      if (error) reject(error);
      else resolve(messages);
    }

    socket.addEventListener("error", () => finish(new Error("MQTT snapshot connection failed")));
    socket.addEventListener("close", () => {
      if (!settled) finish(new Error("MQTT snapshot connection closed"));
    });
    socket.addEventListener("message", async (event) => {
      try {
        buffer = appendBuffer(buffer, await eventBytes(event.data));
        const decoded = decodeFrames(buffer);
        buffer = decoded.rest;
        for (const packet of decoded.packets) {
          const published = parsePublish(packet);
          if (!published) continue;
          messages[published.topic] = published;
          if (messages[requiredTopic] && !settleTimer) {
            settleTimer = setTimeout(() => finish(), settleMs);
          }
        }
      } catch (error) {
        finish(error);
      }
    });

    socket.send(subscribePacket(1, topics));
  });
}

export async function publishRetainedTopics(options, messages) {
  const socket = await openMqttSocket(options);
  for (const [topic, payload] of Object.entries(messages)) {
    socket.send(publishPacket(topic, typeof payload === "string" ? payload : JSON.stringify(payload), { retain: true }));
  }
  await new Promise((resolve) => setTimeout(resolve, 150));
  try { socket.close(1000, "publish complete"); } catch {}
}
