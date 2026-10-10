// auto-fill geolocation on forms with #lat / #lon
function fillGeo() {
  const lat = document.getElementById('lat');
  const lon = document.getElementById('lon');
  if (!lat || !lon) return;
  if (!navigator.geolocation) { alert('Geolocation not supported'); return; }
  navigator.geolocation.getCurrentPosition(
    p => {
      lat.value = p.coords.latitude.toFixed(6);
      lon.value = p.coords.longitude.toFixed(6);
    },
    () => alert('Could not get location. Allow permission and retry.')
  );
}

// live alert refresh on dashboard (#alert-list, data-source=/api/alerts)
(function () {
  const list = document.getElementById('alert-list');
  if (!list) return;
  const src = list.dataset.source || '/api/alerts';
  async function tick() {
    try {
      const r = await fetch(src);
      const items = await r.json();
      const badge = document.getElementById('alert-count');
      if (badge) badge.textContent = items.length;
      if (!items.length) return; // keep server-rendered empty state
      // update "last updated" line only — full re-render stays server-side for reliability
      const upd = document.getElementById('last-updated');
      if (upd) upd.textContent = 'Updated just now · ' + new Date().toLocaleTimeString();
    } catch (e) { /* offline — ignore */ }
  }
  setInterval(tick, 10000);
})();
