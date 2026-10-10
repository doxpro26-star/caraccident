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

// shared accident map (Leaflet + OpenStreetMap, no API key).
// opts: { hospital: {lat,lon,name}|null, hospitals: [...], alerts: [...] }
// Only GPS-fix alerts (lat/lon != 0) are pinned; view auto-fits all pins.
window.renderCrashMap = function (elId, opts) {
  var el = document.getElementById(elId);
  if (!el || typeof L === 'undefined') return;
  opts = opts || {};
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function pin(cls) {
    return L.divIcon({ className: '', html: '<div class="pin ' + cls + '"></div>', iconSize: [16, 16] });
  }
  var pts = [];
  var start = [18.5204, 73.8567]; // fallback centre (Pune)
  if (opts.hospital) start = [opts.hospital.lat, opts.hospital.lon];
  var map = L.map(elId).setView(start, 12);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    maxZoom: 19, attribution: '&copy; <a href="https://osm.org/copyright">OpenStreetMap</a>'
  }).addTo(map);
  if (opts.hospital) {
    var h = opts.hospital;
    pts.push([h.lat, h.lon]);
    L.marker([h.lat, h.lon], { icon: pin('pin-hosp') }).addTo(map)
      .bindPopup('<b>🏥 ' + esc(h.name) + '</b><br>Your hospital');
  }
  (opts.hospitals || []).forEach(function (h) {
    pts.push([h.lat, h.lon]);
    L.marker([h.lat, h.lon], { icon: pin('pin-hosp') }).addTo(map)
      .bindPopup('<b>🏥 ' + esc(h.name) + '</b>');
  });
  (opts.alerts || []).forEach(function (a) {
    if (!a || a.lat === 0 && a.lon === 0) return; // no GPS fix: list only
    pts.push([a.lat, a.lon]);
    L.marker([a.lat, a.lon], { icon: pin('pin-crash') }).addTo(map)
      .bindPopup('<b>🚨 ' + esc(a.reason) + '</b><br>' + esc(a.device_id) +
        ' · ' + esc(a.time) + ' IST<br><a target="_blank" href="https://maps.google.com/?q=' +
        a.lat + ',' + a.lon + '">Navigate →</a>');
  });
  if (pts.length > 1) map.fitBounds(pts, { padding: [30, 30] });
};
