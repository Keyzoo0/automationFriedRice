const HOST = location.hostname;
const POLL_INTERVAL = 500;
const MAX_CHART_POINTS = 60;

let chart;

function initChart() {
  const ctx = document.getElementById('rpmChart').getContext('2d');
  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'RPM',
        data: [],
        borderColor: '#34d399',
        backgroundColor: 'rgba(52, 211, 153, 0.1)',
        fill: true,
        tension: 0.3,
        pointRadius: 0,
        borderWidth: 2,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { display: true, grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#6b7280', maxTicksLimit: 6 } },
        y: { display: true, grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#6b7280' }, beginAtZero: true }
      }
    }
  });
}

function addChartPoint(value) {
  const now = new Date();
  const label = now.getSeconds() + 's';
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(value);
  if (chart.data.labels.length > MAX_CHART_POINTS) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  chart.update('none');
}

// ─── Tab Navigation ──────────────────────────────────────
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => {
      b.classList.remove('active', 'text-emerald-400', 'border-emerald-500');
      b.classList.add('text-gray-500');
    });
    btn.classList.add('active', 'text-emerald-400', 'border-emerald-500');
    btn.classList.remove('text-gray-500');
    document.querySelectorAll('.tab-content').forEach(tc => tc.classList.add('hidden'));
    document.getElementById('tab-' + btn.dataset.tab).classList.remove('hidden');
  });
});

// ─── Fetch Status ────────────────────────────────────────
async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    updateDashboard(d);
  } catch (e) {
    document.getElementById('statusText').textContent = 'OFFLINE';
    document.getElementById('statusLed').className = 'w-3 h-3 rounded-full bg-red-500';
  }
}

function updateDashboard(d) {
  // Status
  const running = d.is_running;
  const statusText = document.getElementById('statusText');
  const runLed = document.getElementById('runLed');
  const statusLed = document.getElementById('statusLed');

  statusText.textContent = running ? 'RUN' : 'STOP';
  statusText.className = 'text-2xl font-bold ' + (running ? 'text-emerald-400' : 'text-gray-500');
  runLed.className = 'w-4 h-4 rounded-full ' + (running ? 'bg-emerald-500 on' : 'bg-gray-600');
  statusLed.className = 'w-3 h-3 rounded-full ' + (running ? 'bg-emerald-500 on' : 'bg-gray-600');

  // RPM & Setpoint
  document.getElementById('setpointVal').textContent = d.setpoint ? d.setpoint.toFixed(1) : '--';
  document.getElementById('rpmVal').textContent = d.current_rpm ? d.current_rpm.toFixed(1) : '0.0';

  // Chart
  addChartPoint(d.current_rpm || 0);

  // Modbus
  const mb = document.getElementById('modbusBadge');
  mb.textContent = d.modbus_ok ? 'Modbus: OK' : 'Modbus: FAIL';
  mb.className = 'text-xs px-2 py-1 rounded-full ' + (d.modbus_ok ? 'bg-emerald-900/50 text-emerald-400' : 'bg-red-900/50 text-red-400');

  // PID
  document.getElementById('pidOutVal').textContent = (d.pid_output || 0).toFixed(0);
  document.getElementById('vfdFreqVal').textContent = d.vfd_freq_raw || 0;

  // Fill fields if not focused
  const kpField = document.getElementById('pidKp');
  const kiField = document.getElementById('pidKi');
  const kdField = document.getElementById('pidKd');
  if (document.activeElement !== kpField) kpField.value = d.kp?.toFixed(2) || '0.23';
  if (document.activeElement !== kiField) kiField.value = d.ki?.toFixed(2) || '0.30';
  if (document.activeElement !== kdField) kdField.value = d.kd?.toFixed(1) || '21.4';

  // SP positions
  const sp1 = document.querySelector('.sp-input[data-pos="1"]');
  const sp2 = document.querySelector('.sp-input[data-pos="2"]');
  const sp3 = document.querySelector('.sp-input[data-pos="3"]');
  if (document.activeElement !== sp1) sp1.value = d.sp_pos1?.toFixed(1) || '30';
  if (document.activeElement !== sp2) sp2.value = d.sp_pos2?.toFixed(1) || '25';
  if (document.activeElement !== sp3) sp3.value = d.sp_pos3?.toFixed(1) || '20';

  // Uptime
  const uptime = d.uptime_ms || 0;
  const sec = Math.floor(uptime / 1000);
  const min = Math.floor(sec / 60);
  document.getElementById('uptimeVal').textContent = min > 0 ? `${min}m ${sec % 60}s` : `${sec}s`;

  // Heap
  document.getElementById('heapVal').textContent = d.heap || '--';

  // Buttons
  document.getElementById('btnStart').disabled = running;
  document.getElementById('btnStop').disabled = !running;
}

// ─── Controls ────────────────────────────────────────────
document.getElementById('btnStart').addEventListener('click', () => {
  fetch('/api/start');
});

document.getElementById('btnStop').addEventListener('click', () => {
  fetch('/api/stop');
});

document.getElementById('btnApplyPID').addEventListener('click', () => {
  const kp = document.getElementById('pidKp').value;
  const ki = document.getElementById('pidKi').value;
  const kd = document.getElementById('pidKd').value;
  fetch(`/api/pid?kp=${kp}&ki=${ki}&kd=${kd}`);
});

document.getElementById('btnSaveSP').addEventListener('click', () => {
  document.querySelectorAll('.sp-input').forEach(inp => {
    const pos = inp.dataset.pos;
    const val = inp.value;
    fetch(`/api/setpoint?pos=${pos}&value=${val}`);
  });
});

// ─── Init ─────────────────────────────────────────────────
initChart();
setInterval(fetchStatus, POLL_INTERVAL);
fetchStatus();
