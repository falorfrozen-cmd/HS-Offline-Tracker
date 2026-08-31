<script>
  import { invoke } from './bridge.js';
  import { listen, native } from './bridge.js';
  import { RARITIES, soundUrl, play, preload, invalidate } from './audio.js';
  import { fmt } from './format.js';

  let snap = $state(null);

  let cfg = $state(null);
  let locked = $derived(cfg?.locked ?? false);
  /// Whether the cursor is over the strip down the right-hand edge, as the
  /// backend sees it.
  ///
  /// The only source: a locked overlay is sent no mouse events, so :hover can
  /// arrive and never leave, and an unlocked one has no way of knowing what
  /// happened while it was locked. One report, both states.
  let nearStrip = $state(false);
  let drag = $derived(cfg?.locked ? null : '');
  const urls = {};
  const DROP_KEYS = new Set(['satanic', 'set', 'heroic', 'angelic', 'unholy']);
  const DROP_RANK = { set: 1, satanic: 2, heroic: 3, angelic: 4, unholy: 5 };
  const lastPlayed = {};
  let audioReady = false;
  // A drop can arrive in the short gap between mounting this window and the
  // sound files finishing their preload. Keep the most important one rather
  // than turning startup timing into a lost alert. It is replayed through the
  // normal policy once settings and audio are ready, so filters still win.
  let warmingDrop = null;
  let pendingDrop = null;
  let pendingDropTimer;
  let lastDropAt = 0;

  async function initSounds() {
    const settings = await invoke('get_settings').catch(() => null);
    if (!settings) return;
    cfg = settings;
    await warmSounds(settings).catch(() => {});
    audioReady = true;
    if (warmingDrop) {
      const waiting = warmingDrop;
      warmingDrop = null;
      playSound(waiting.key, waiting.rarity);
    }
  }

  async function warmSounds(settings) {
    const active = (settings?.filters ?? []).find((f) => f.id === settings?.filter);
    const keys = [...RARITIES, ...(active?.lists ?? []).map((list) => `list-${list.id}`)];
    const resolved = await Promise.all(keys.map(async (key) => [key, await soundUrl(key)]));
    for (const [key, url] of resolved) urls[key] = url;
    await preload(resolved.map(([, url]) => url));
  }

  // a list brings its own sound and its own volume; everything else is one of
  // the built-in alerts. Lists live inside the active filter — the loose
  // `lists` field is pre-0.9.4 and is emptied by the migration on load.
  function channel(key) {
    if (!key.startsWith('list-')) return cfg?.[key];
    const active = (cfg?.filters ?? []).find((f) => f.id === cfg?.filter);
    return (active?.lists ?? []).find((l) => `list-${l.id}` === key);
  }

  async function playImmediate(key) {
    if (!audioReady || !cfg) return;
    const c = channel(key);
    if (c && c.enabled === false) return;
    if (!(key in urls)) urls[key] = await soundUrl(key);
    play(urls[key], c?.volume ?? 0.7, key);
  }

  function flushDropSound() {
    const chosen = pendingDrop;
    pendingDrop = null;
    pendingDropTimer = null;
    if (!chosen || !audioReady || !cfg) return;

    const now = Date.now();
    const cooldown = chosen.rarity === 'angelic' || chosen.rarity === 'unholy' ? 600 : 400;
    if (now - lastDropAt < 150 || now - (lastPlayed[chosen.rarity] ?? 0) < cooldown) return;
    lastDropAt = now;
    lastPlayed[chosen.rarity] = now;
    void (async () => {
      if (!(chosen.key in urls)) urls[chosen.key] = await soundUrl(chosen.key);
      if (!(chosen.rarity in urls)) urls[chosen.rarity] = await soundUrl(chosen.rarity);
      // A list without a sound of its own borrows the actual item's rarity.
      const url = urls[chosen.key] ?? urls[chosen.rarity];
      play(url, chosen.volume, chosen.rarity, chosen.key);
    })();
  }

  function playSound(key, rarity) {
    if (key === 'mail' || key === 'zone') {
      void playImmediate(key);
      return;
    }
    const rarityKey = String(rarity ?? key).toLowerCase();
    if (!DROP_KEYS.has(rarityKey)) return;
    const rank = (key.startsWith('list-') ? 100 : 0) + (DROP_RANK[rarityKey] ?? 0);
    if (!audioReady || !cfg) {
      const candidate = { key, rarity: rarityKey, rank };
      if (!warmingDrop || candidate.rank > warmingDrop.rank) warmingDrop = candidate;
      return;
    }
    const c = channel(key);
    const label = rarityKey[0].toUpperCase() + rarityKey.slice(1);
    // Rarity alert state has one authority: settings.alerts. Legacy
    // settings.<rarity>.enabled is deliberately ignored here.
    if (!key.startsWith('list-') && !(cfg.alerts ?? []).includes(label)) return;
    if (key.startsWith('list-') && c?.enabled === false) return;

    const candidate = { key, rarity: rarityKey, volume: c?.volume ?? 0.7, rank };
    if (!pendingDrop || candidate.rank > pendingDrop.rank) pendingDrop = candidate;
    if (!pendingDropTimer) pendingDropTimer = setTimeout(flushDropSound, 90);
  }

  // the backend pushes a snapshot when something changes; the clock is kept
  // running locally so the seconds never stutter between two pushes
  let clock = $state({ secs: 0, at: Date.now() });
  let tick = $state(Date.now());
  let sessionSecs = $derived(
    Math.max(0, clock.secs + (snap?.paused ? 0 : Math.floor((tick - clock.at) / 1000))),
  );

  function received(s) {
    const now = Date.now();
    snap = s;
    clock = { secs: s.session_secs, at: now };
    tick = now;
  }

  // Whether the mail is news rather than a standing fact. Paired with has_mail
  // in the markup rather than cleared by an effect, so collecting the mail stops
  // the blink without this ever having to watch the snapshot.
  let mailFresh = $state(false);
  let mailTimer;

  $effect(() => {
    let disposed = false;
    let statsRevision = 0;
    initSounds();
    const unsubs = [
      listen('stats', (e) => {
        statsRevision += 1;
        received(e.payload);
      }),
      // this window is the one that plays sounds, hidden or not — the backend
      // says when mail arrives rather than leaving it to be spotted in a
      // snapshot that only travels to windows on screen
      listen('mail', () => {
        playSound('mail');
        // The chime is easy to miss with the game's own sound up, and the chip
        // said "Mail!" in the same bone as every other figure on the panel. It
        // blinks while the news is new and then settles to a gold that stays
        // until the mail is collected — mail waits as long as you leave it, and
        // a blink that never stops is a blink you learn to ignore.
        mailFresh = true;
        clearTimeout(mailTimer);
        mailTimer = setTimeout(() => (mailFresh = false), 20000);
      }),
      listen('item-drop', (e) => playSound(...(Array.isArray(e.payload) ? e.payload : [e.payload]))),
      // The rotation, from the backend, which only says so for a real one —
      // never for the zone this app has just learned about, and never for one
      // whose buffs the player did not ask to hear about. That last is decided
      // there rather than here: the pillar asks the same question, and a rule
      // written out twice in two languages is a rule that comes to disagree.
      //
      // Chime and pulse answer to the one switch: they are two halves of the
      // same alert. The pillar has its own, because being shown and being told
      // are not the same want.
      listen('zone-changed', () => {
        if (cfg?.zone?.enabled === false) return;
        playSound('zone');
      }),
      listen('settings-changed', (e) => {
        cfg = e.payload;
        void warmSounds(cfg);
      }),
      listen('strip-hover', (e) => {
        nearStrip = !!e.payload;
      }),
      listen('sounds-changed', async (e) => {
        invalidate(urls[e.payload]);
        urls[e.payload] = await soundUrl(e.payload);
        await preload([urls[e.payload]]);
      }),
    ];
    // Subscribe before taking the initial snapshot. If a live push arrives
    // while that read is in flight, its revision wins over the older reply.
    Promise.all(unsubs).then(() => {
      const revision = statsRevision;
      invoke('snapshot').then((initial) => {
        if (!disposed && statsRevision === revision) received(initial);
      }).catch(() => {});
    }).catch(() => {});
    const timer = setInterval(() => (tick = Date.now()), 1000);
    return () => {
      disposed = true;
      clearInterval(timer);
      clearTimeout(mailTimer);
      clearTimeout(pendingDropTimer);
      unsubs.forEach((u) => u.then((f) => f()));
    };
  });


  function dur(secs) {
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    const s = secs % 60;
    return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  }

  const item = (name) => snap?.items?.[name] ?? { total: 0, mf: 0, per_hour: 0 };
  const gain = (n) => (Number(n) > 0 ? `+${fmt(n)}` : fmt(n));
  const DROP_METRICS = [
    { key: 'Satanic', short: 'SAT', cls: 'sat' },
    { key: 'Set', short: 'SET', cls: 'set' },
    { key: 'Heroic', short: 'HERO', cls: 'hero' },
    { key: 'Angelic', short: 'ANG', cls: 'angel' },
    { key: 'Unholy', short: 'UNH', cls: 'unholy' },
  ];

  // The window is only ever as tall as the panel inside it. Measuring here and
  // telling the backend keeps the two in step whatever rows are switched on —
  // and means adding a row to this file needs nothing done anywhere else.
  // `bind:this` writes this, and an effect reads it. As a plain `let` that
  // works by the order the two happen to run in; behind an `{#if}` it would
  // stop working with nothing to see — the overlay would simply never resize
  // itself again.
  let panelEl = $state(null);
  $effect(() => {
    if (!panelEl) return;
    const report = () => {
      const { width, height } = panelEl.getBoundingClientRect();
      // The width goes with it now. It used to be a constant on both sides, and
      // on a machine whose text came out wider the chips — fixed widths, no
      // wrapping — spilled over the row instead of the panel giving way. See
      // "Squished Panel".
      if (height > 0) invoke('fit_overlay', { height, width }).catch(() => {});
    };
    const observer = new ResizeObserver(report);
    observer.observe(panelEl);
    report();
    return () => observer.disconnect();
  });

  let status = $derived.by(() => {
    if (!snap) return { cls: 'pending', label: 'CONNECTING', tip: 'Connecting to the local session reader' };
    if (snap.paused) return { cls: 'paused', label: 'PAUSED', tip: 'Session clock paused' };
    const s = snap?.status ?? '';
    if (s.startsWith('offline-live|'))
      return { cls: 'ok', label: 'LIVE', tip: `Local event stream live · ${Number(s.split('|')[1] || 0).toLocaleString('en-GB')} events` };
    if (s.startsWith('offline-progress|'))
      return { cls: 'save', label: 'SAVE', tip: `Save-only mode · kills/XP are delayed; live Gold and drops need the local sensor` };
    if (s.startsWith('offline-waiting|'))
      return { cls: 'warn', label: 'WAITING', tip: 'Game found · waiting for stable save progress or the local Season 10 bridge' };
    if (s.startsWith('offline-error|'))
      return { cls: 'err', label: 'ERROR', tip: s.split('|').slice(1).join('|') || 'Local event bridge error' };
    if (s === 'waiting-for-game') return { cls: 'warn', label: 'WAITING', tip: 'Waiting for Hero Siege to start' };
    return { cls: 'err', label: 'OFFLINE', tip: s || 'Unknown local event-reader state' };
  });

  const shown = (id) => !(cfg?.hidden ?? []).includes(id);

  // The strip, under the lock. These were the right-click menu, which was a menu
  // nobody could find: nothing on the panel ever hinted it was there.
  const ENTRIES = [
    { key: 'dashboard', title: 'Open dashboard', run: () => invoke('full_mode') },
    { key: 'hide', title: 'Hide to tray', run: () => invoke('hide_window') },
  ];




  async function toggleLock() {
    if (!cfg) cfg = await invoke('get_settings').catch(() => null);
    if (!cfg) return;
    cfg = { ...cfg, locked: !cfg.locked };
    invoke('save_settings', { settings: cfg }).catch(() => {});
  }
</script>

<div
  bind:this={panelEl}
  class="panel"
  class:loading={!snap}
  style:--surface-alpha={cfg?.opacity ?? 1}
  data-tauri-drag-region={drag}
>
  {#if !snap}
    <div class="connecting" data-tauri-drag-region={drag}>
      <span class="connect-mark" aria-hidden="true"></span>
      <div class="connect-copy" data-tauri-drag-region={drag}>
        <strong>Connecting</strong>
        <span>Waiting for local session data</span>
      </div>
    </div>
  {:else}
    <header class="topbar" data-tauri-drag-region={drag}>
      <div class="session-state" title={status.tip} data-tauri-drag-region={drag}>
        <span class="status-light {status.cls}" aria-hidden="true"></span>
        <span class="status-label">{status.label}</span>
        <span class="timer" class:paused={snap.paused}>{dur(sessionSecs)}</span>
      </div>
      <div class="header-badges" data-tauri-drag-region={drag}>
        {#if shown('session') && snap.has_mail}
          <span class="mail-badge" class:fresh={mailFresh} title="Mail is waiting">
            <svg viewBox="0 0 20 20" aria-hidden="true">
              <rect x="2.5" y="4" width="15" height="12" rx="2"></rect>
              <path d="m4 6 6 4.5L16 6"></path>
            </svg>
            MAIL
          </span>
        {/if}
        {#if shown('items')}
          <span class="ss-badge" title="SS drops this session">
            <span>SS</span>
            <strong>{fmt(snap.ss)}</strong>
          </span>
        {/if}
      </div>
    </header>

    <section class="metrics" aria-label="Session performance" data-tauri-drag-region={drag}>
      {#if shown('gold')}
        <div class="metric gold" data-tauri-drag-region={drag}>
          <span class="metric-label">Gold</span>
          <strong>{gain(snap.gold?.earned)}</strong>
          <span class="metric-rate">{fmt(snap.gold?.per_hour)}/h</span>
        </div>
      {/if}
      {#if shown('xp')}
        <div class="metric xp" data-tauri-drag-region={drag}>
          <span class="metric-label">XP</span>
          <strong>{gain(snap.xp?.earned)}</strong>
          <span class="metric-rate">{fmt(snap.xp?.per_hour)}/h</span>
        </div>
      {/if}
      <div class="metric kills" data-tauri-drag-region={drag}>
        <span class="metric-label">Kills</span>
        <strong>{gain(snap.kills?.earned)}</strong>
        <span class="metric-rate">{fmt(snap.kills?.per_hour)}/h</span>
      </div>
    </section>

    {#if shown('items')}
      <section class="drops" aria-label="Drop counts" data-tauri-drag-region={drag}>
        {#each DROP_METRICS as drop}
          <div class="drop-count {drop.cls}" title={`${drop.key} drops`} data-tauri-drag-region={drag}>
            <span>{drop.short}</span>
            <strong>{fmt(item(drop.key).total)}</strong>
          </div>
        {/each}
      </section>
    {/if}
  {/if}
</div>

<!-- The control rail stays outside the opacity-controlled reading surface. -->
{#if native}
  <div class="strip" class:near={nearStrip} class:free={!locked}>
    <button
      class="cell lock"
      onclick={toggleLock}
      title={locked
        ? 'Locked — click to unlock (Ctrl+Shift+L)'
        : 'Lock and make the overlay click-through (Ctrl+Shift+L)'}
      aria-label={locked ? 'unlock the overlay' : 'lock the overlay'}
    >
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <rect x="5" y="10" width="14" height="10" rx="2"></rect>
        {#if locked}
          <path d="M8 10V7a4 4 0 0 1 8 0v3"></path>
        {:else}
          <path d="M8 10V7a4 4 0 0 1 7.6-1.7"></path>
        {/if}
      </svg>
    </button>
    {#each ENTRIES as entry}
      <button class="cell" onclick={entry.run} title={entry.title} aria-label={entry.title}>
        <svg viewBox="0 0 24 24" aria-hidden="true">
          {#if entry.key === 'dashboard'}
            <rect x="4" y="4" width="6" height="6" rx="1"></rect>
            <rect x="14" y="4" width="6" height="6" rx="1"></rect>
            <rect x="4" y="14" width="6" height="6" rx="1"></rect>
            <rect x="14" y="14" width="6" height="6" rx="1"></rect>
          {:else}
            <path d="M5 12h14M8 9l-3 3 3 3"></path>
          {/if}
        </svg>
      </button>
    {/each}
  </div>
{/if}

<style>
  :global(html, body) {
    margin: 0;
    background: transparent;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
    cursor: default;
  }

  :global(#app) { min-height: 100%; }

  .panel {
    --surface-alpha: 1;
    position: relative;
    box-sizing: border-box;
    width: 444px;
    min-height: 147px;
    padding: 10px 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    overflow: hidden;
    color: var(--bone-12);
    background: rgb(6 10 17 / var(--surface-alpha));
    border: 1px solid rgb(86 136 154 / 0.48);
    border-radius: 12px;
    box-shadow:
      inset 0 1px 0 rgb(255 255 255 / 0.06),
      inset 0 -18px 36px rgb(0 0 0 / 0.14);
    font-family: "Segoe UI Variable", "Segoe UI", system-ui, sans-serif;
    font-size: 12px;
    font-variant-numeric: tabular-nums;
    letter-spacing: 0.01em;
  }

  .panel::before {
    content: '';
    position: absolute;
    inset: 0 18px auto;
    height: 1px;
    background: linear-gradient(90deg, transparent, var(--arcane), transparent);
    opacity: 0.48;
    pointer-events: none;
  }

  .panel.loading { justify-content: center; }

  .connecting {
    min-height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 12px;
    color: var(--bone-8);
  }

  .connect-mark {
    width: 20px;
    height: 20px;
    flex: none;
    border: 2px solid rgb(89 214 192 / 0.2);
    border-top-color: var(--arcane);
    border-radius: 50%;
    animation: connect-spin 0.9s linear infinite;
  }

  .connect-copy {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .connect-copy strong {
    color: var(--bone-13);
    font-size: 13px;
    font-weight: 650;
  }

  .connect-copy span {
    color: var(--bone-5);
    font-size: 10px;
    letter-spacing: 0.04em;
  }

  @keyframes connect-spin { to { transform: rotate(360deg); } }

  .topbar {
    min-height: 26px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
  }

  .session-state,
  .header-badges,
  .mail-badge,
  .ss-badge {
    display: flex;
    align-items: center;
  }

  .session-state {
    min-width: 0;
    gap: 7px;
  }

  .status-light {
    width: 7px;
    height: 7px;
    flex: none;
    border-radius: 50%;
    background: var(--dim-2);
    box-shadow: 0 0 0 3px rgb(120 136 154 / 0.08);
  }

  .status-light.ok { background: #57d68a; box-shadow: 0 0 10px rgb(87 214 138 / 0.45); }
  .status-light.save { background: #62b7e8; box-shadow: 0 0 10px rgb(98 183 232 / 0.36); }
  .status-light.warn { background: #e5b85d; box-shadow: 0 0 10px rgb(229 184 93 / 0.34); }
  .status-light.err { background: #ef6a76; box-shadow: 0 0 10px rgb(239 106 118 / 0.4); }
  .status-light.paused { background: #86a9c8; }

  .status-label {
    color: var(--bone-7);
    font-size: 9px;
    font-weight: 700;
    letter-spacing: 0.12em;
  }

  .timer {
    min-width: 64px;
    padding-left: 9px;
    border-left: 1px solid rgb(255 255 255 / 0.09);
    color: var(--bone-14);
    font-size: 14px;
    font-weight: 650;
    line-height: 1;
  }

  .timer.paused { color: #a9c8e2; }

  .header-badges {
    justify-content: flex-end;
    gap: 6px;
  }

  .header-badges:empty { display: none; }

  .mail-badge,
  .ss-badge {
    height: 22px;
    border: 1px solid rgb(255 255 255 / 0.1);
    border-radius: 7px;
    background: rgb(255 255 255 / 0.035);
    font-size: 9px;
    font-weight: 700;
    letter-spacing: 0.08em;
  }

  .mail-badge {
    gap: 4px;
    padding: 0 7px;
    color: var(--gold-2);
    border-color: rgb(240 198 110 / 0.28);
  }

  .mail-badge svg {
    width: 12px;
    height: 12px;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.5;
  }

  .mail-badge.fresh { animation: mail-alert 1.1s ease-in-out infinite; }

  @keyframes mail-alert {
    50% {
      border-color: rgb(240 198 110 / 0.7);
      box-shadow: 0 0 12px rgb(240 198 110 / 0.18);
    }
  }

  .ss-badge {
    gap: 5px;
    padding: 0 7px 0 6px;
  }

  .ss-badge span { color: var(--gold-2); }
  .ss-badge strong {
    color: var(--bone-14);
    font-size: 11px;
    letter-spacing: 0;
  }

  .metrics {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(108px, 1fr));
    gap: 6px;
  }

  .metric {
    --metric-accent: var(--edge-8);
    min-width: 0;
    height: 48px;
    padding: 6px 8px 7px;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    grid-template-rows: auto 1fr;
    align-items: end;
    column-gap: 6px;
    border: 1px solid rgb(255 255 255 / 0.075);
    border-radius: 8px;
    background: rgb(255 255 255 / 0.028);
    box-shadow: inset 0 2px 0 var(--metric-accent);
  }

  .metric.gold { --metric-accent: var(--gold-1); }
  .metric.xp { --metric-accent: var(--mf); }
  .metric.kills { --metric-accent: var(--arcane); }

  .metric-label {
    grid-column: 1 / -1;
    align-self: start;
    color: var(--bone-5);
    font-size: 8px;
    font-weight: 700;
    line-height: 1;
    letter-spacing: 0.13em;
    text-transform: uppercase;
  }

  .metric strong {
    min-width: 0;
    overflow: hidden;
    color: var(--bone-14);
    font-size: 13px;
    font-weight: 650;
    line-height: 1;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .metric-rate {
    color: var(--bone-5);
    font-size: 9px;
    line-height: 1;
    white-space: nowrap;
  }

  .drops {
    height: 34px;
    display: grid;
    grid-template-columns: repeat(5, minmax(0, 1fr));
    gap: 5px;
  }

  .drop-count {
    --drop: var(--bone-7);
    min-width: 0;
    padding: 4px 6px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 5px;
    border: 1px solid rgb(255 255 255 / 0.075);
    border-top-color: var(--drop);
    border-radius: 7px;
    background: rgb(255 255 255 / 0.025);
  }

  .drop-count.sat { --drop: var(--rar-satanic); }
  .drop-count.set { --drop: var(--rar-set); }
  .drop-count.hero { --drop: var(--rar-heroic); }
  .drop-count.angel { --drop: var(--rar-angelic); }
  .drop-count.unholy { --drop: var(--rar-unholy); }

  .drop-count span {
    overflow: hidden;
    color: var(--drop);
    font-size: 8px;
    font-weight: 750;
    letter-spacing: 0.08em;
    text-overflow: clip;
  }

  .drop-count strong {
    overflow: hidden;
    color: var(--bone-13);
    font-size: 12px;
    font-weight: 650;
    text-overflow: ellipsis;
  }

  /* The native backend owns click-through. This small sibling remains fully
     legible even when the reading surface's background alpha is reduced. */
  .strip {
    position: fixed;
    top: 0;
    right: 0;
    width: 28px;
    height: 91px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2px;
    z-index: 2;
    opacity: 0;
    pointer-events: none;
    transition: opacity 120ms ease-out;
  }

  .strip.free,
  .strip.near { transition: none; }

  .strip.free { opacity: 0.72; pointer-events: auto; }
  .strip.near { opacity: 1; pointer-events: auto; }

  .cell {
    width: 28px;
    height: 28px;
    flex: none;
    padding: 0;
    display: grid;
    place-items: center;
    color: var(--bone-8);
    background: rgb(7 12 20 / 0.94);
    border: 1px solid rgb(86 136 154 / 0.48);
    border-radius: 7px;
    cursor: pointer;
  }

  .cell.lock {
    margin-bottom: 3px;
    color: var(--gold-2);
  }

  .cell:hover {
    color: var(--bone-15);
    background: rgb(18 31 44 / 0.98);
    border-color: var(--arcane);
  }

  .cell:active { transform: translateY(1px); }

  .cell svg {
    width: 17px;
    height: 17px;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.6;
    stroke-linecap: round;
    stroke-linejoin: round;
    pointer-events: none;
  }

  @media (prefers-reduced-motion: reduce) {
    .connect-mark,
    .mail-badge.fresh { animation: none; }
  }
</style>
