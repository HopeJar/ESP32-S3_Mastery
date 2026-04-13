import * as THREE from "/assets/vendor/three.module.min.js";

const MAP_URL = "/assets/maps/q2dm1.bsp";
const DEBUG_PIXELS = new URLSearchParams(window.location.search).has("debug-pixels");
const BSP_IDENT = 0x50534249;
const BSP_VERSION = 38;
const LUMPS = {
  entities: 0,
  vertices: 2,
  texInfo: 5,
  faces: 6,
  edges: 11,
  surfEdges: 12,
};

const elements = {
  menu: document.getElementById("menu"),
  game: document.getElementById("game"),
  canvas: document.getElementById("game-canvas"),
  status: document.getElementById("status"),
  serverTitle: document.getElementById("server-title"),
  siteNameLabel: document.getElementById("site-name-label"),
  networkState: document.getElementById("network-state"),
  matchSummary: document.getElementById("match-summary"),
  joinForm: document.getElementById("join-form"),
  networkForm: document.getElementById("network-form"),
  adminSetupForm: document.getElementById("admin-setup-form"),
  adminLoginForm: document.getElementById("admin-login-form"),
  adminSettingsForm: document.getElementById("admin-settings-form"),
  adminState: document.getElementById("admin-state"),
  adminBadge: document.getElementById("admin-badge"),
  playerName: document.getElementById("player-name"),
  adminSetupPassword: document.getElementById("admin-setup-password"),
  adminSetupConfirm: document.getElementById("admin-setup-confirm"),
  adminLoginPassword: document.getElementById("admin-login-password"),
  siteName: document.getElementById("site-name"),
  serverName: document.getElementById("server-name"),
  matchMode: document.getElementById("match-mode"),
  maxPlayers: document.getElementById("max-players"),
  timeLimit: document.getElementById("time-limit"),
  fragLimit: document.getElementById("frag-limit"),
  teamScoreLimit: document.getElementById("team-score-limit"),
  friendlyFire: document.getElementById("friendly-fire"),
  networkMode: document.getElementById("network-mode"),
  staSsid: document.getElementById("sta-ssid"),
  staPassword: document.getElementById("sta-password"),
  apSsid: document.getElementById("ap-ssid"),
  apPassword: document.getElementById("ap-password"),
  rebootButton: document.getElementById("reboot-button"),
  matchLabel: document.getElementById("match-label"),
  playerCount: document.getElementById("player-count"),
  scoreLabel: document.getElementById("score-label"),
  positionLabel: document.getElementById("position-label"),
};

const state = {
  renderer: null,
  scene: null,
  camera: null,
  map: null,
  player: null,
  peers: new Map(),
  keys: new Set(),
  yaw: 0,
  pitch: 0,
  lastFrame: 0,
  lastSync: 0,
  syncInFlight: false,
  running: false,
  adminPassword: "",
  settings: {
    site_name: "espquake.local",
    server_name: "ESP Quake",
    match_mode: "ffa",
    max_players: 8,
    time_limit_min: 20,
    frag_limit: 30,
    team_score_limit: 50,
    friendly_fire: false,
  },
};

function setStatus(message) {
  if (!elements.status) return;
  elements.status.textContent = message || "";
  elements.status.classList.toggle("hidden", !message);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
  });
  const text = await response.text();
  const data = text ? JSON.parse(text) : {};
  if (!response.ok || data.ok === false) {
    throw new Error(data.error || response.statusText);
  }
  return data;
}

function settingNumber(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function normalizeSettings(settings = {}) {
  return {
    site_name: settings.site_name || "espquake.local",
    server_name: settings.server_name || "ESP Quake",
    match_mode: settings.match_mode === "teams" ? "teams" : "ffa",
    max_players: Math.min(Math.max(settingNumber(settings.max_players, 8), 1), 8),
    time_limit_min: Math.min(Math.max(settingNumber(settings.time_limit_min, 20), 0), 240),
    frag_limit: Math.min(Math.max(settingNumber(settings.frag_limit, 30), 0), 999),
    team_score_limit: Math.min(Math.max(settingNumber(settings.team_score_limit, 50), 0), 999),
    friendly_fire: Boolean(settings.friendly_fire),
  };
}

function settingsFromForm() {
  return normalizeSettings({
    site_name: elements.siteName.value.trim(),
    server_name: elements.serverName.value.trim(),
    match_mode: elements.matchMode.value,
    max_players: elements.maxPlayers.value,
    time_limit_min: elements.timeLimit.value,
    frag_limit: elements.fragLimit.value,
    team_score_limit: elements.teamScoreLimit.value,
    friendly_fire: elements.friendlyFire.checked,
  });
}

function applySettings(settings) {
  state.settings = normalizeSettings(settings);
  elements.siteName.value = state.settings.site_name;
  elements.serverName.value = state.settings.server_name;
  elements.matchMode.value = state.settings.match_mode;
  elements.maxPlayers.value = state.settings.max_players;
  elements.timeLimit.value = state.settings.time_limit_min;
  elements.fragLimit.value = state.settings.frag_limit;
  elements.teamScoreLimit.value = state.settings.team_score_limit;
  elements.friendlyFire.checked = state.settings.friendly_fire;
  elements.serverTitle.textContent = state.settings.server_name;
  elements.siteNameLabel.textContent = state.settings.site_name;
  elements.matchSummary.textContent = `${state.settings.match_mode === "teams" ? "Teams" : "Free for All"} q2dm1, ${state.settings.max_players} players, ${state.settings.time_limit_min || "no"} min, ${state.settings.frag_limit || "no"} frag limit`;
  elements.matchLabel.textContent = `q2dm1 ${state.settings.match_mode}`;
  elements.scoreLabel.textContent = `0 / ${state.settings.match_mode === "teams" ? state.settings.team_score_limit : state.settings.frag_limit}`;
}

function showAdminState(configured, unlocked) {
  elements.adminSetupForm.hidden = configured;
  elements.adminLoginForm.hidden = !configured || unlocked;
  elements.adminSettingsForm.hidden = configured && !unlocked;
  elements.networkForm.querySelectorAll("input, select, button").forEach((control) => {
    control.disabled = configured && !unlocked;
  });
  elements.adminBadge.textContent = configured ? (unlocked ? "Unlocked" : "Locked") : "Unclaimed";
  elements.adminState.textContent = configured
    ? (unlocked ? "Match controls are unlocked." : "Enter the admin password to change match controls.")
    : "First connected player can claim admin.";
}

async function loadAdminSettings() {
  const data = await api("/api/v1/admin");
  applySettings(data.settings);
  showAdminState(Boolean(data.admin_configured), !data.admin_configured);
}

function toQ2Vector(vector) {
  return { x: vector.x, y: vector.z * -1, z: vector.y };
}

function fromQ2Vector(vector) {
  return new THREE.Vector3(vector.x, vector.z, -vector.y);
}

function decodeText(view, offset, length) {
  const bytes = new Uint8Array(view.buffer, view.byteOffset + offset, length);
  const end = bytes.indexOf(0);
  return new TextDecoder("utf-8").decode(end >= 0 ? bytes.subarray(0, end) : bytes);
}

function getLump(view, index) {
  const offset = 8 + index * 8;
  return {
    offset: view.getInt32(offset, true),
    length: view.getInt32(offset + 4, true),
  };
}

function parseEntities(text) {
  const spawns = [];
  const objectPattern = /\{([^}]*)\}/g;
  let objectMatch = objectPattern.exec(text);

  while (objectMatch) {
    const entity = {};
    const pairPattern = /"([^"]+)"\s+"([^"]*)"/g;
    let pairMatch = pairPattern.exec(objectMatch[1]);
    while (pairMatch) {
      entity[pairMatch[1]] = pairMatch[2];
      pairMatch = pairPattern.exec(objectMatch[1]);
    }

    if (entity.classname === "info_player_deathmatch" && entity.origin) {
      const parts = entity.origin.trim().split(/\s+/).map(Number);
      if (parts.length === 3 && parts.every(Number.isFinite)) {
        spawns.push({
          position: fromQ2Vector({ x: parts[0], y: parts[1], z: parts[2] + 32 }),
          yaw: THREE.MathUtils.degToRad(Number(entity.angle || 0)),
        });
      }
    }

    objectMatch = objectPattern.exec(text);
  }

  return spawns;
}

function hashText(value) {
  let hash = 2166136261;
  for (let i = 0; i < value.length; i += 1) {
    hash ^= value.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function textureColor(name) {
  const hash = hashText(name || "q2");
  const color = new THREE.Color();
  color.setHSL((hash % 360) / 360, 0.42, 0.48);
  return color;
}

function parseQ2Bsp(buffer) {
  const view = new DataView(buffer);
  if (view.getUint32(0, true) !== BSP_IDENT || view.getInt32(4, true) !== BSP_VERSION) {
    throw new Error("q2dm1.bsp is not a Quake 2 BSP v38 map.");
  }

  const vertexLump = getLump(view, LUMPS.vertices);
  const edgeLump = getLump(view, LUMPS.edges);
  const surfEdgeLump = getLump(view, LUMPS.surfEdges);
  const faceLump = getLump(view, LUMPS.faces);
  const texInfoLump = getLump(view, LUMPS.texInfo);
  const entityLump = getLump(view, LUMPS.entities);

  const vertices = [];
  for (let offset = vertexLump.offset; offset < vertexLump.offset + vertexLump.length; offset += 12) {
    vertices.push({
      x: view.getFloat32(offset, true),
      y: view.getFloat32(offset + 4, true),
      z: view.getFloat32(offset + 8, true),
    });
  }

  const edges = [];
  for (let offset = edgeLump.offset; offset < edgeLump.offset + edgeLump.length; offset += 4) {
    edges.push([view.getUint16(offset, true), view.getUint16(offset + 2, true)]);
  }

  const surfEdges = [];
  for (let offset = surfEdgeLump.offset; offset < surfEdgeLump.offset + surfEdgeLump.length; offset += 4) {
    surfEdges.push(view.getInt32(offset, true));
  }

  const texInfos = [];
  for (let offset = texInfoLump.offset; offset < texInfoLump.offset + texInfoLump.length; offset += 76) {
    texInfos.push(decodeText(view, offset + 40, 32));
  }

  const positions = [];
  const colors = [];
  const bounds = new THREE.Box3();

  for (let offset = faceLump.offset; offset < faceLump.offset + faceLump.length; offset += 20) {
    const firstEdge = view.getInt32(offset + 4, true);
    const edgeCount = view.getUint16(offset + 8, true);
    const texInfoIndex = view.getUint16(offset + 10, true);
    if (edgeCount < 3 || edgeCount > 64) {
      continue;
    }

    const polygon = [];
    for (let i = 0; i < edgeCount; i += 1) {
      const surfEdge = surfEdges[firstEdge + i];
      const edge = edges[Math.abs(surfEdge)];
      if (!edge) continue;

      const vertex = vertices[surfEdge >= 0 ? edge[0] : edge[1]];
      if (!vertex) continue;

      const point = fromQ2Vector(vertex);
      polygon.push(point);
      bounds.expandByPoint(point);
    }

    if (polygon.length < 3) {
      continue;
    }

    const color = textureColor(texInfos[texInfoIndex]);
    for (let i = 1; i < polygon.length - 1; i += 1) {
      for (const point of [polygon[0], polygon[i], polygon[i + 1]]) {
        positions.push(point.x, point.y, point.z);
        colors.push(color.r, color.g, color.b);
      }
    }
  }

  if (positions.length === 0 || bounds.isEmpty()) {
    throw new Error("q2dm1.bsp did not produce drawable geometry.");
  }

  const entitiesText = decodeText(view, entityLump.offset, entityLump.length);
  const spawns = parseEntities(entitiesText);
  if (spawns.length === 0) {
    const center = new THREE.Vector3();
    bounds.getCenter(center);
    center.y = bounds.min.y + 96;
    spawns.push({ position: center, yaw: 0 });
  }

  return { positions, colors, bounds, spawns };
}

function createMapMesh(map) {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(map.positions, 3));
  geometry.setAttribute("color", new THREE.Float32BufferAttribute(map.colors, 3));
  geometry.computeVertexNormals();

  const material = new THREE.MeshLambertMaterial({
    vertexColors: true,
    side: THREE.DoubleSide,
  });

  return new THREE.Mesh(geometry, material);
}

function resizeRenderer() {
  if (!state.renderer || !state.camera) return;

  const width = elements.canvas.clientWidth || window.innerWidth;
  const height = elements.canvas.clientHeight || window.innerHeight;
  state.renderer.setSize(width, height, false);
  state.camera.aspect = width / Math.max(1, height);
  state.camera.updateProjectionMatrix();
}

async function loadMap() {
  if (state.map) return state.map;

  setStatus("Loading q2dm1.bsp...");
  const response = await fetch(MAP_URL);
  if (!response.ok) {
    throw new Error("q2dm1.bsp was not available from the ESP32 asset partition.");
  }

  const parsed = parseQ2Bsp(await response.arrayBuffer());
  state.scene = new THREE.Scene();
  state.scene.background = new THREE.Color(0x050505);
  state.scene.fog = new THREE.Fog(0x050505, 1600, 6200);

  const mapMesh = createMapMesh(parsed);
  state.scene.add(mapMesh);
  state.scene.add(new THREE.HemisphereLight(0xffffff, 0x151515, 1.9));

  const keyLight = new THREE.DirectionalLight(0xffffff, 1.4);
  keyLight.position.set(400, 900, 600);
  state.scene.add(keyLight);

  state.camera = new THREE.PerspectiveCamera(75, 1, 4, 12000);
  state.camera.rotation.order = "YXZ";

  state.renderer = new THREE.WebGLRenderer({
    canvas: elements.canvas,
    antialias: true,
    powerPreference: "high-performance",
    preserveDrawingBuffer: DEBUG_PIXELS,
  });
  state.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  state.renderer.outputColorSpace = THREE.SRGBColorSpace;

  window.addEventListener("resize", resizeRenderer);
  resizeRenderer();

  state.map = parsed;
  setStatus("");
  return parsed;
}

function spawnLocalPlayer() {
  const spawnIndex = state.player?.spawn_index || 0;
  const spawn = state.map.spawns[spawnIndex % state.map.spawns.length];
  state.camera.position.copy(spawn.position);
  state.yaw = spawn.yaw;
  state.pitch = 0;
  state.camera.rotation.set(state.pitch, state.yaw, 0);
}

function peerColor(color) {
  return new THREE.Color(color || 0x3ddc84);
}

function ensurePeer(player) {
  let peer = state.peers.get(player.id);
  if (peer) return peer;

  const group = new THREE.Group();
  const body = new THREE.Mesh(
    new THREE.CapsuleGeometry(18, 48, 4, 10),
    new THREE.MeshStandardMaterial({ color: peerColor(player.color), roughness: 0.65 })
  );
  const marker = new THREE.Mesh(
    new THREE.ConeGeometry(10, 26, 12),
    new THREE.MeshStandardMaterial({ color: 0xf2f2f2, roughness: 0.5 })
  );

  body.position.y = 24;
  marker.position.set(0, 48, -22);
  marker.rotation.x = Math.PI / 2;
  group.add(body, marker);
  state.scene.add(group);
  state.peers.set(player.id, group);
  return group;
}

function syncPeers(players) {
  const seen = new Set();
  for (const player of players || []) {
    if (!state.player || player.id === state.player.id) {
      continue;
    }

    seen.add(player.id);
    const peer = ensurePeer(player);
    peer.position.set(player.x, player.y, player.z);
    peer.rotation.y = player.yaw || 0;
  }

  for (const [id, peer] of state.peers) {
    if (!seen.has(id)) {
      state.scene.remove(peer);
      state.peers.delete(id);
    }
  }

  const count = players?.length || 1;
  elements.playerCount.textContent = `${count} / ${state.settings.max_players} player${state.settings.max_players === 1 ? "" : "s"}`;
}

function bindControls() {
  elements.canvas.tabIndex = 0;
  elements.canvas.addEventListener("click", () => {
    elements.canvas.focus();
    elements.canvas.requestPointerLock?.();
  });

  window.addEventListener("keydown", (event) => {
    state.keys.add(event.code);
    if (["KeyW", "KeyA", "KeyS", "KeyD", "Space", "ControlLeft", "ShiftLeft"].includes(event.code)) {
      event.preventDefault();
    }
    if (event.code === "KeyR" && state.map) {
      spawnLocalPlayer();
    }
  }, { passive: false });

  window.addEventListener("keyup", (event) => {
    state.keys.delete(event.code);
  });

  window.addEventListener("mousemove", (event) => {
    if (document.pointerLockElement !== elements.canvas) return;
    state.yaw -= event.movementX * 0.0024;
    state.pitch -= event.movementY * 0.0024;
    state.pitch = THREE.MathUtils.clamp(state.pitch, -1.42, 1.42);
  });
}

function moveLocalPlayer(deltaSeconds) {
  const speed = state.keys.has("ShiftLeft") ? 720 : 420;
  const amount = speed * deltaSeconds;

  state.camera.rotation.set(state.pitch, state.yaw, 0);

  const forward = new THREE.Vector3();
  state.camera.getWorldDirection(forward);
  forward.y = 0;
  if (forward.lengthSq() > 0) forward.normalize();

  const right = new THREE.Vector3(forward.z, 0, -forward.x);
  if (state.keys.has("KeyW")) state.camera.position.addScaledVector(forward, amount);
  if (state.keys.has("KeyS")) state.camera.position.addScaledVector(forward, -amount);
  if (state.keys.has("KeyD")) state.camera.position.addScaledVector(right, amount);
  if (state.keys.has("KeyA")) state.camera.position.addScaledVector(right, -amount);
  if (state.keys.has("Space")) state.camera.position.y += amount;
  if (state.keys.has("ControlLeft") || state.keys.has("KeyC")) state.camera.position.y -= amount;

  const q2 = toQ2Vector(state.camera.position);
  elements.positionLabel.textContent = `${Math.round(q2.x)} ${Math.round(q2.y)} ${Math.round(q2.z)}`;
}

async function syncNetwork(force = false) {
  if (!state.player || state.syncInFlight) return;

  const now = performance.now();
  if (!force && now - state.lastSync < 125) return;

  state.lastSync = now;
  state.syncInFlight = true;
  try {
    const position = state.camera.position;
    const data = await api("/api/v1/player/state", {
      method: "POST",
      body: JSON.stringify({
        id: state.player.id,
        x: position.x,
        y: position.y,
        z: position.z,
        yaw: state.yaw,
        pitch: state.pitch,
      }),
    });
    if (data.match) {
      applySettings({
        ...state.settings,
        server_name: data.match.server_name || state.settings.server_name,
        match_mode: data.match.mode || state.settings.match_mode,
        max_players: data.match.max_players || state.settings.max_players,
        time_limit_min: data.match.time_limit_min ?? state.settings.time_limit_min,
        frag_limit: data.match.frag_limit ?? state.settings.frag_limit,
        team_score_limit: data.match.team_score_limit ?? state.settings.team_score_limit,
        friendly_fire: data.match.friendly_fire ?? state.settings.friendly_fire,
      });
    }
    syncPeers(data.players);
  } catch (error) {
    console.error(error);
    setStatus(error instanceof Error ? error.message : "Network sync failed.");
  } finally {
    state.syncInFlight = false;
  }
}

function frame(now) {
  if (!state.running) return;

  const deltaSeconds = Math.min((now - state.lastFrame) / 1000 || 0, 0.05);
  state.lastFrame = now;

  moveLocalPlayer(deltaSeconds);
  state.renderer.render(state.scene, state.camera);
  void syncNetwork();
  requestAnimationFrame(frame);
}

async function enterMatch(name) {
  setStatus("Joining match...");
  const join = await api("/api/v1/match/join", {
    method: "POST",
    body: JSON.stringify({ name }),
  });

  state.player = join.player;
  if (join.match) {
    applySettings({
      ...state.settings,
      match_mode: join.match.mode || state.settings.match_mode,
      max_players: join.match.max_players || state.settings.max_players,
    });
  }
  elements.menu.hidden = true;
  elements.game.hidden = false;

  await loadMap();
  spawnLocalPlayer();
  bindControls();
  state.running = true;
  state.lastFrame = performance.now();
  setStatus("Click the match view for mouse look.");
  await syncNetwork(true);
  requestAnimationFrame(frame);
}

async function loadNetworkSettings() {
  const data = await api("/api/v1/network");
  const network = data.network || {};
  elements.networkMode.value = network.mode || "apsta";
  elements.staSsid.value = network.ssid || "";
  elements.apSsid.value = network.ap_ssid || "";
  elements.networkState.textContent = `${network.ap_started ? "AP" : ""}${network.ap_started && network.sta_connected ? " + " : ""}${network.sta_connected ? "STA" : ""}` || "Offline";
  if (network.site_name && !state.settings.site_name) {
    elements.siteNameLabel.textContent = network.site_name;
  }
}

function bindForms() {
  elements.joinForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const name = elements.playerName.value.trim() || "ranger";
    enterMatch(name).catch((error) => {
      console.error(error);
      setStatus(error instanceof Error ? error.message : "Join failed.");
      elements.menu.hidden = false;
      elements.game.hidden = true;
    });
  });

  elements.adminSetupForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const password = elements.adminSetupPassword.value;
    if (password.length < 8) {
      setStatus("Admin password needs at least 8 characters.");
      return;
    }
    if (password !== elements.adminSetupConfirm.value) {
      setStatus("Admin passwords do not match.");
      return;
    }

    api("/api/v1/admin/setup", {
      method: "POST",
      body: JSON.stringify({
        admin_password: password,
        ...settingsFromForm(),
      }),
    }).then((data) => {
      state.adminPassword = password;
      elements.adminSetupPassword.value = "";
      elements.adminSetupConfirm.value = "";
      applySettings(data.settings);
      showAdminState(true, true);
      setStatus("Admin claimed. Reboot to apply local site name changes.");
    }).catch((error) => {
      console.error(error);
      setStatus(error instanceof Error ? error.message : "Admin setup failed.");
    });
  });

  elements.adminLoginForm.addEventListener("submit", (event) => {
    event.preventDefault();
    const password = elements.adminLoginPassword.value;
    api("/api/v1/admin/login", {
      method: "POST",
      body: JSON.stringify({ admin_password: password }),
    }).then((data) => {
      state.adminPassword = password;
      elements.adminLoginPassword.value = "";
      applySettings(data.settings);
      showAdminState(true, true);
      setStatus("Admin controls unlocked.");
    }).catch((error) => {
      console.error(error);
      setStatus(error instanceof Error ? error.message : "Admin login failed.");
    });
  });

  elements.adminSettingsForm.addEventListener("submit", (event) => {
    event.preventDefault();
    if (!state.adminPassword) {
      setStatus("Unlock admin controls first.");
      return;
    }

    api("/api/v1/admin/settings", {
      method: "POST",
      body: JSON.stringify({
        admin_password: state.adminPassword,
        ...settingsFromForm(),
      }),
    }).then((data) => {
      applySettings(data.settings);
      setStatus("Admin settings saved. Reboot to apply local site name changes.");
    }).catch((error) => {
      console.error(error);
      setStatus(error instanceof Error ? error.message : "Admin settings save failed.");
    });
  });

  elements.networkForm.addEventListener("submit", (event) => {
    event.preventDefault();
    api("/api/v1/network", {
      method: "POST",
      body: JSON.stringify({
        mode: elements.networkMode.value,
        ssid: elements.staSsid.value.trim(),
        password: elements.staPassword.value,
        ap_ssid: elements.apSsid.value.trim() || "ESPQUAKE",
        ap_password: elements.apPassword.value,
        admin_password: state.adminPassword,
      }),
    }).then(() => {
      elements.staPassword.value = "";
      elements.apPassword.value = "";
      setStatus("Network saved. Reboot to apply it.");
    }).catch((error) => {
      console.error(error);
      setStatus(error instanceof Error ? error.message : "Network save failed.");
    });
  });

  elements.rebootButton.addEventListener("click", () => {
    api("/api/v1/network/reboot", {
      method: "POST",
      body: JSON.stringify({ admin_password: state.adminPassword }),
    })
      .then(() => setStatus("Rebooting..."))
      .catch((error) => {
        console.error(error);
        setStatus(error instanceof Error ? error.message : "Reboot failed.");
      });
  });
}

async function init() {
  bindForms();
  try {
    await loadAdminSettings();
    await loadNetworkSettings();
    setStatus("");
  } catch (error) {
    console.error(error);
    setStatus(error instanceof Error ? error.message : "Setup API is not available.");
  }
}

init();
