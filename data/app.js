// FSAE Lap Timer dashboard client.
// Fetches initial state over REST, then stays live via WebSocket push.

const bestLapValueEl = document.getElementById("bestLapValue");
const connectionStatusEl = document.getElementById("connectionStatus");
const gatesTableBody = document.querySelector("#gatesTable tbody");
const lapsTableBody = document.querySelector("#lapsTable tbody");
const resetSessionBtn = document.getElementById("resetSessionBtn");

function formatLapTime(us) {
  if (!us || us <= 0) return "--:--.---";
  const ms = Math.round(us / 1000);
  const minutes = Math.floor(ms / 60000);
  const seconds = Math.floor((ms % 60000) / 1000);
  const millis = ms % 1000;
  return `${minutes}:${String(seconds).padStart(2, "0")}.${String(millis).padStart(3, "0")}`;
}

function renderLaps(session) {
  const bestLapUs = session.bestLapTimeUs || 0;
  bestLapValueEl.textContent = formatLapTime(bestLapUs);

  lapsTableBody.innerHTML = "";
  const laps = session.laps || [];
  // Newest lap first.
  for (let i = laps.length - 1; i >= 0; i--) {
    const lap = laps[i];
    const row = document.createElement("tr");
    if (lap.isBestLap) row.classList.add("best-lap");

    const sectorsText = (lap.sectorTimesUs || []).map(formatLapTime).join(" / ") || "--";
    row.innerHTML = `<td>${lap.lapNumber}</td><td>${formatLapTime(lap.lapTimeUs)}</td><td>${sectorsText}</td>`;
    lapsTableBody.appendChild(row);
  }
}

function renderGates(gates) {
  gatesTableBody.innerHTML = "";
  for (const gate of gates) {
    const row = document.createElement("tr");
    row.classList.add(gate.online ? "gate-online" : "gate-offline");
    const battery = (gate.batteryMillivolts / 1000).toFixed(2) + " V";
    const lastSeenSec = Math.round((gate.lastSeenMsAgo || 0) / 1000);
    row.innerHTML = `<td>Gate ${gate.gateId}</td><td class="gate-status">${
      gate.online ? "Online" : "Offline"
    }</td><td>${battery}</td><td>${lastSeenSec}s ago</td>`;
    gatesTableBody.appendChild(row);
  }
}

async function refreshSession() {
  const res = await fetch("/api/session");
  if (res.ok) renderLaps(await res.json());
}

async function refreshGates() {
  const res = await fetch("/api/gates");
  if (res.ok) renderGates(await res.json());
}

function setConnectionStatus(connected) {
  connectionStatusEl.textContent = connected ? "live" : "disconnected";
  connectionStatusEl.classList.toggle("status-connected", connected);
  connectionStatusEl.classList.toggle("status-disconnected", !connected);
}

function connectWebSocket() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${proto}//${location.host}/ws`);

  ws.onopen = () => setConnectionStatus(true);
  ws.onclose = () => {
    setConnectionStatus(false);
    setTimeout(connectWebSocket, 2000); // simple auto-reconnect
  };
  ws.onerror = () => ws.close();

  ws.onmessage = (evt) => {
    let msg;
    try {
      msg = JSON.parse(evt.data);
    } catch (e) {
      return;
    }

    switch (msg.type) {
      case "lap":
      case "sessionReset":
      case "gateOrderChanged":
        refreshSession();
        break;
      case "gateStatus":
        renderGates(msg.gates || []);
        break;
      default:
        break;
    }
  };
}

resetSessionBtn.addEventListener("click", async () => {
  if (!confirm("Reset the current session? This clears all recorded laps.")) return;
  await fetch("/api/session/reset", { method: "POST" });
  refreshSession();
});

refreshSession();
refreshGates();
connectWebSocket();
