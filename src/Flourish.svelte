<script>
  // A click-through stack for the few drops that deserve the player's eye.
  // The journal keeps every find; this window only needs to make the important
  // ones readable now, without parking a bright plate across the fight.
  import { invoke, listen, native } from './bridge.js';
  import { itemName, tierLabel, typeLabel } from './items.js';
  import { buffInfo, zoneName } from './buffs.js';

  const MAX_TOASTS = 3;
  const GROUP_MS = 1500;
  const IN_MS = 180;
  const OUT_MS = 260;

  // One order owns overflow. A full stack may forget a Set to make room for an
  // Unholy, never the other way around.
  const PRIORITY = { Set: 1, Satanic: 2, Heroic: 3, Angelic: 4, Unholy: 5 };
  const LIFE_MS = { Set: 3200, Satanic: 3500, Heroic: 4000, Angelic: 4500, Unholy: 5000 };
  const RARITY_TINT = {
    Satanic: 'var(--rar-satanic, #ff5d72)',
    Set: 'var(--rar-set, #5bd873)',
    Heroic: 'var(--rar-heroic, #54e0ce)',
    Angelic: 'var(--rar-angelic, #f2d36f)',
    Unholy: 'var(--rar-unholy, #c16cff)',
  };
  const ZONE_TINT = 'var(--rar-satanic, #ff5d72)';

  const SAMPLE = {
    rarity: 'Heroic',
    name: "Fenrir's Bloodfang",
    tier: 6,
    item_type: 3,
    weapon_type: 1,
  };
  const ZONE_SAMPLE = {
    kind: 'zone',
    zone: 'Satanic_5_5',
    buffs: [2, 14, 21],
    debuffs: [3, 9],
  };

  let toasts = $state([]);
  let placing = $state(false);
  let cfg = $state(null);
  let nextId = 0;
  let latestRevision = 0;
  const timers = new Map();

  const stopPlacing = () => invoke('place_flourish', { placing: false }).catch(() => {});

  function rarityOf(entry) {
    const raw = String(entry?.rarity ?? 'Drop');
    return Object.keys(PRIORITY).find((name) => name.toLowerCase() === raw.toLowerCase()) ?? raw;
  }

  function labelOf(entry) {
    if (!entry || entry.kind === 'zone') return '';
    const named = String(entry.name ?? '').trim();
    if (named) return named;
    const known = itemName(entry.item_type, entry.item_id, entry.weapon_type);
    return known ?? typeLabel(entry.item_type, entry.weapon_type);
  }

  function zoneDetails(entry) {
    const names = (entry?.buffs ?? []).map((id) => buffInfo(id).name).filter(Boolean);
    if (!names.length) return 'No buffs this rotation';
    const shown = names.slice(0, 2).join(' · ');
    return names.length > 2 ? `${shown} · +${names.length - 2}` : shown;
  }

  function tintOf(toast) {
    return toast.kind === 'zone'
      ? ZONE_TINT
      : (RARITY_TINT[toast.rarity] ?? 'var(--arcane, #59d6c0)');
  }

  // Duration is a ceiling, not a reason every rarity should occupy the screen
  // for six or twelve seconds. Common alerts clear first; the two rarest get
  // enough time to be read from the corner of the eye.
  function lifeOf(entry) {
    const cap = Math.round(Math.min(12, Math.max(2, cfg?.flourish_secs ?? 6)) * 1000);
    const wanted = entry.kind === 'zone' ? 5000 : (LIFE_MS[rarityOf(entry)] ?? 3600);
    return Math.min(cap, wanted);
  }

  function makeToast(entry, preview = false) {
    const zone = entry.kind === 'zone';
    const rarity = zone ? 'Satanic Zone' : rarityOf(entry);
    const label = zone ? zoneName(entry.zone) : labelOf(entry);
    const tier = Number(entry.tier ?? 0);
    const group = zone
      ? `zone:${entry.zone ?? ''}:${(entry.buffs ?? []).join(',')}`
      : `${rarity.toLowerCase()}|${label.toLowerCase()}|${tier}`;
    return {
      ...entry,
      id: ++nextId,
      preview,
      kind: zone ? 'zone' : 'drop',
      rarity,
      label,
      tier,
      group,
      count: 1,
      priority: zone ? 6 : (PRIORITY[rarity] ?? 0),
      lastSeen: Date.now(),
      leaving: false,
    };
  }

  function clearToastTimers(id) {
    const own = timers.get(id);
    if (!own) return;
    clearTimeout(own.fade);
    clearTimeout(own.remove);
    timers.delete(id);
  }

  function finishIfEmpty() {
    // The window knows when its last independently-timed card has gone, so it
    // keeps the existing contract: the web side tells the backend when to hide.
    if (!toasts.length && !placing && native) {
      invoke('flourish_done', { revision: latestRevision }).catch(() => {});
    }
  }

  function removeToast(id, finish = true) {
    clearToastTimers(id);
    toasts = toasts.filter((toast) => toast.id !== id);
    if (finish) finishIfEmpty();
  }

  function schedule(toast, life) {
    clearToastTimers(toast.id);
    const fade = setTimeout(() => {
      toasts = toasts.map((item) =>
        item.id === toast.id ? { ...item, leaving: true } : item
      );
    }, Math.max(IN_MS, life - OUT_MS));
    const remove = setTimeout(() => removeToast(toast.id), life);
    timers.set(toast.id, { fade, remove });
  }

  function clearToasts() {
    for (const toast of toasts) clearToastTimers(toast.id);
    toasts = [];
  }

  function placementSamples() {
    clearToasts();
    toasts = [makeToast(SAMPLE, true), makeToast(ZONE_SAMPLE, true)];
  }

  function enqueue(entry) {
    if (!entry || placing) return;
    latestRevision = Math.max(latestRevision, Number(entry._flourish_revision ?? 0));
    const now = Date.now();
    const candidate = makeToast(entry);

    // Real repeated drops remain real drops. They become a count on the card
    // instead of the second one being suppressed or obscuring the first.
    const grouped = toasts.find((toast) =>
      !toast.preview && toast.group === candidate.group && now - toast.lastSeen <= GROUP_MS
    );
    if (grouped) {
      const renewed = {
        ...grouped,
        ...entry,
        count: grouped.count + 1,
        lastSeen: now,
        leaving: false,
      };
      toasts = toasts.map((toast) => toast.id === grouped.id ? renewed : toast);
      schedule(renewed, lifeOf(renewed));
      return;
    }

    if (toasts.length >= MAX_TOASTS) {
      // Once the stack is full, only a genuinely higher-priority drop may
      // replace a card. Equal/weaker item rain stays in the journal instead of
      // continuously rebuilding and animating the overlay DOM.
      const weakest = [...toasts].sort((a, b) =>
        a.priority - b.priority || a.lastSeen - b.lastSeen
      )[0];
      if (candidate.priority <= weakest.priority) return;
      removeToast(weakest.id, false);
    }

    toasts = [...toasts, candidate];
    schedule(candidate, lifeOf(candidate));
  }

  // While placement is active this window takes the mouse. Escape must always
  // end it or the transparent drag surface would trap clicks underneath.
  $effect(() => {
    const key = (event) => {
      if (event.key === 'Escape' && placing) stopPlacing();
    };
    window.addEventListener('keydown', key);
    return () => window.removeEventListener('keydown', key);
  });

  $effect(() => {
    invoke('get_settings').then((settings) => (cfg = settings)).catch(() => {});
    if (!native) {
      // Browser development gets a stable, timer-free sample. The packaged
      // app never enters this branch; real cards still come only from events.
      toasts = [
        makeToast({ ...SAMPLE, rarity: 'Unholy', name: 'Oath of the Hollow Star' }, true),
        makeToast(SAMPLE, true),
        makeToast({ ...SAMPLE, rarity: 'Angelic', name: 'Astral Covenant', tier: 5 }, true),
      ];
    }
    const unsubs = [
      listen('settings-changed', (event) => (cfg = event.payload)),
      listen('flourish-play', (event) => enqueue(event.payload)),
      listen('flourish-placing', (event) => {
        placing = Boolean(event.payload);
        if (placing) placementSamples();
        else clearToasts();
      }),
    ];
    return () => {
      clearToasts();
      unsubs.forEach((unsubscribe) => unsubscribe.then((fn) => fn()));
    };
  });

  let scale = $derived(Math.min(1.5, Math.max(0.75, cfg?.flourish_scale ?? 1)));
  const surface = 0.9;
</script>

<div class="stage" style:--scale={scale} style:--surface={surface}>
  <!-- The native window scales with the setting. Keeping one fixed design
       canvas inside it means spacing and the three-card fit do not drift at
       50% or 200%. -->
  <div class="canvas" class:placing>
    <div class="stack" aria-live={placing ? 'off' : 'polite'}>
      {#each toasts as toast (toast.id)}
        <div
          class="toast"
          class:zone={toast.kind === 'zone'}
          class:leaving={toast.leaving}
          class:preview={toast.preview}
          style:--tint={tintOf(toast)}
          title={toast.label}
        >
          <span class="rarity-edge" aria-hidden="true"></span>
          <span class="mark" aria-hidden="true"><span></span></span>

          <span class="copy">
            <span class="item-name">{toast.label}</span>
            {#if toast.kind === 'zone'}
              <span class="meta">
                <span class="rarity">Satanic Zone</span>
                <span class="dot">·</span>
                <span class="zone-detail">{zoneDetails(toast)}</span>
              </span>
            {:else}
              <span class="meta">
                <span class="rarity">{toast.rarity}</span>
                {#if toast.tier > 0}
                  <span class="dot">·</span>
                  <span class="tier">Tier {tierLabel(toast.tier)}</span>
                {/if}
              </span>
            {/if}
          </span>

          {#if toast.count > 1}
            <span class="count" aria-label={`${toast.count} drops`}>×{toast.count}</span>
          {/if}
        </div>
      {/each}
    </div>

    {#if placing}
      <!-- Placement remains a bounded, visible interaction: the whole surface
           drags, Escape always exits, and the samples show both supported card
           shapes without an animation loop. -->
      <div class="place" data-tauri-drag-region>
        <div class="place-hint" data-tauri-drag-region>
          <span class="place-dot"></span>
          Drag notification area
        </div>
        <button class="done" onclick={stopPlacing}>Done <span>· Esc</span></button>
      </div>
    {/if}
  </div>
</div>

<style>
  @font-face {
    font-family: 'Offline Tracker UI';
    src: local('Segoe UI Variable'), local('Segoe UI Semibold'), local('Segoe UI');
    font-weight: 600 750;
  }

  :global(html, body, #app) {
    width: 100%;
    height: 100%;
    margin: 0;
    background: transparent;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
  }

  .stage {
    position: relative;
    width: 100vw;
    height: 100vh;
    overflow: hidden;
    font-family: 'Offline Tracker UI', 'Segoe UI', sans-serif;
    color: var(--bone-12, #eaf1f6);
  }

  /* The backend sizes the native surface from the same scale. The fixed canvas
     then scales into it exactly, keeping the three 58px cards inside 220px. */
  .canvas {
    position: absolute;
    inset: 0 auto auto 0;
    width: 420px;
    height: 220px;
    transform: scale(var(--scale));
    transform-origin: top left;
  }

  .stack {
    position: absolute;
    inset: 12px;
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    justify-content: flex-end;
    gap: 8px;
    pointer-events: none;
  }

  .canvas.placing .stack {
    inset: 42px 14px;
  }

  .toast {
    position: relative;
    box-sizing: border-box;
    width: 396px;
    max-width: 100%;
    height: 58px;
    flex: 0 0 58px;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 12px 8px 16px;
    overflow: hidden;
    border: 1px solid color-mix(in srgb, var(--tint) 26%, #263442);
    border-radius: 10px;
    background:
      linear-gradient(105deg, color-mix(in srgb, var(--tint) 10%, transparent), transparent 38%),
      rgb(5 9 14 / var(--surface));
    box-shadow:
      0 10px 28px rgb(0 0 0 / 0.48),
      0 0 18px color-mix(in srgb, var(--tint) 15%, transparent),
      inset 0 1px rgb(255 255 255 / 0.045);
    isolation: isolate;
    animation: toast-in 180ms cubic-bezier(0.2, 0.85, 0.25, 1) both;
  }

  .toast::after {
    content: '';
    position: absolute;
    inset: 0;
    z-index: -1;
    pointer-events: none;
    background: linear-gradient(180deg, rgb(255 255 255 / 0.028), transparent 42%);
  }

  .toast.leaving {
    animation: toast-out 260ms ease-in both;
  }

  .toast.preview {
    animation: none;
  }

  .rarity-edge {
    position: absolute;
    inset: -1px auto -1px -1px;
    width: 4px;
    border-radius: 10px 0 0 10px;
    background: var(--tint);
    box-shadow: 0 0 14px color-mix(in srgb, var(--tint) 58%, transparent);
  }

  .mark {
    position: relative;
    width: 28px;
    height: 28px;
    flex: 0 0 28px;
    display: grid;
    place-items: center;
    border: 1px solid color-mix(in srgb, var(--tint) 34%, #253443);
    border-radius: 8px;
    background: color-mix(in srgb, var(--tint) 8%, #09111a);
    box-shadow: inset 0 0 10px rgb(0 0 0 / 0.42);
  }

  .mark > span {
    width: 8px;
    height: 8px;
    border: 1px solid var(--tint);
    transform: rotate(45deg);
    background: color-mix(in srgb, var(--tint) 48%, transparent);
    box-shadow: 0 0 8px color-mix(in srgb, var(--tint) 60%, transparent);
  }

  .zone .mark > span {
    width: 10px;
    height: 10px;
    border-radius: 50%;
    transform: none;
    background: transparent;
    box-shadow:
      0 0 0 3px color-mix(in srgb, var(--tint) 14%, transparent),
      0 0 10px color-mix(in srgb, var(--tint) 55%, transparent);
  }

  .zone .mark::before,
  .zone .mark::after {
    content: '';
    position: absolute;
    width: 14px;
    height: 1px;
    background: var(--tint);
    opacity: 0.75;
  }

  .zone .mark::after {
    transform: rotate(90deg);
  }

  .copy {
    min-width: 0;
    flex: 1;
    display: flex;
    flex-direction: column;
    justify-content: center;
    gap: 3px;
  }

  .item-name {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--bone-14, #f6f9fb);
    font-size: 15px;
    font-weight: 720;
    line-height: 1.08;
    letter-spacing: 0.005em;
    text-shadow: 0 1px 2px #000;
  }

  .meta {
    min-width: 0;
    display: flex;
    align-items: center;
    gap: 5px;
    overflow: hidden;
    color: var(--bone-5, #91a4b8);
    font-size: 10px;
    font-weight: 700;
    line-height: 1;
    letter-spacing: 0.075em;
    text-transform: uppercase;
  }

  .rarity {
    flex: none;
    color: var(--tint);
  }

  .dot {
    flex: none;
    color: var(--edge-7, #49778b);
  }

  .tier,
  .zone-detail {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .zone-detail {
    color: var(--bone-6, #a3b6c8);
    letter-spacing: 0.035em;
    text-transform: none;
  }

  .count {
    min-width: 32px;
    height: 28px;
    flex: 0 0 auto;
    display: grid;
    place-items: center;
    padding: 0 7px;
    box-sizing: border-box;
    border: 1px solid color-mix(in srgb, var(--tint) 40%, #253443);
    border-radius: 8px;
    background: color-mix(in srgb, var(--tint) 10%, #081019);
    color: var(--tint);
    font-size: 12px;
    font-weight: 750;
    font-variant-numeric: tabular-nums;
    box-shadow: 0 0 12px color-mix(in srgb, var(--tint) 10%, transparent);
    animation: count-in 140ms ease-out both;
  }

  @keyframes toast-in {
    from { opacity: 0; transform: translateX(16px) scale(0.985); }
    to { opacity: 1; transform: translateX(0) scale(1); }
  }

  @keyframes toast-out {
    from { opacity: 1; transform: translateX(0) scale(1); }
    to { opacity: 0; transform: translateX(12px) scale(0.985); }
  }

  @keyframes count-in {
    from { opacity: 0; transform: scale(0.82); }
    to { opacity: 1; transform: scale(1); }
  }

  /* Placement is visible without painting over the samples. Its two compact
     controls occupy the otherwise empty strips above and below them. */
  .place {
    position: absolute;
    inset: 1px;
    z-index: 10;
    box-sizing: border-box;
    border: 1px dashed rgb(163 182 200 / 0.7);
    border-radius: 12px;
    cursor: move;
  }

  .place-hint,
  .done {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
    height: 28px;
    box-sizing: border-box;
    display: flex;
    align-items: center;
    justify-content: center;
    border: 1px solid var(--edge-4, #31516a);
    border-radius: 8px;
    background: rgb(5 9 14 / 0.94);
    color: var(--bone-10, #d8e2ea);
    box-shadow: 0 5px 16px rgb(0 0 0 / 0.45);
    font-size: 11px;
    font-weight: 700;
    white-space: nowrap;
  }

  .place-hint {
    top: 7px;
    gap: 7px;
    padding: 0 12px;
    letter-spacing: 0.035em;
    text-transform: uppercase;
  }

  .place-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--arcane, #59d6c0);
    box-shadow: 0 0 8px color-mix(in srgb, var(--arcane, #59d6c0) 65%, transparent);
  }

  .done {
    bottom: 7px;
    min-width: 116px;
    padding: 0 14px;
    cursor: pointer;
    font: inherit;
    font-size: 11px;
    font-weight: 720;
  }

  .done span {
    margin-left: 4px;
    color: var(--bone-5, #91a4b8);
    font-size: 10px;
  }

  .done:hover {
    border-color: var(--arcane, #59d6c0);
    color: white;
  }

  .done:focus-visible {
    outline: 2px solid var(--arcane, #59d6c0);
    outline-offset: 2px;
  }

  /* WebKitGTK's transparent X11 surface accumulates translucent animation
     frames. A hard arrival/departure keeps the dark card dark instead of
     painting it repeatedly into a black rectangle. */
  :global(html[data-os='linux']) .toast {
    animation: none;
  }

  :global(html[data-os='linux']) .toast.leaving {
    animation: none;
    opacity: 0;
  }

  @media (prefers-reduced-motion: reduce) {
    .toast,
    .toast.leaving,
    .count {
      animation: none !important;
      transition: none !important;
    }

    .toast.leaving {
      opacity: 0;
    }
  }
</style>
