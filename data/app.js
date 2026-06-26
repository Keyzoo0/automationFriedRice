const HOST = location.hostname;
const POLL_MS = 500;
const MAX_POINTS = 60;
const RECONNECT_DELAY = 2000;

let chart = null;
let fetchFailCount = 0;
let lastStatus = null;

// ─── Toast ────────────────────────────────────────────────
function toast(msg, ok = true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = `fixed bottom-4 right-4 border rounded-xl px-4 py-3 shadow-2xl text-xs transition-all duration-300 z-50 max-w-xs ${ok ? 'bg-emerald-900/80 border-emerald-700 text-emerald-200' : 'bg-red-900/80 border-red-700 text-red-200'}`;
  el.classList.remove('translate-y-20', 'opacity-0');
  el.classList.add('translate-y-0', 'opacity-100');
  clearTimeout(el._hide);
  el._hide = setTimeout(() => {
    el.classList.add('translate-y-20', 'opacity-0');
    el.classList.remove('translate-y-0', 'opacity-100');
  }, 3000);
}

// ─── Chart ─────────────────────────────────────────────────
function initChart() {
  const ctx = document.getElementById('rpmChart');
  if (!ctx) return;
  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'RPM',
        data: [],
        borderColor: '#34d399',
        backgroundColor: (ctx) => {
          const g = ctx.chart.ctx.createLinearGradient(0, 0, 0, ctx.chart.height);
          g.addColorStop(0, 'rgba(52,211,153,0.25)');
          g.addColorStop(1, 'rgba(52,211,153,0.01)');
          return g;
        },
        fill: true,
        tension: 0.4,
        pointRadius: 0,
        borderWidth: 2,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { intersect: false, mode: 'index' },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: '#1e293b',
          titleColor: '#94a3b8',
          bodyColor: '#fff',
          borderColor: '#334155',
          borderWidth: 1,
          padding: 8,
          bodyFont: { size: 12 },
          callbacks: {
            label: ctx => `${ctx.parsed.y.toFixed(1)} RPM`
          }
        }
      },
      scales: {
        x: {
          display: true,
          grid: { color: 'rgba(255,255,255,0.04)' },
          ticks: { color: '#475569', maxTicksLimit: 8, font: { size: 9 } }
        },
        y: {
          display: true,
          grid: { color: 'rgba(255,255,255,0.04)' },
          ticks: { color: '#475569', font: { size: 9 } },
          beginAtZero: true
        }
      }
    }
  });
}

function addChartPoint(value) {
  if (!chart) return;
  const t = new Date();
  chart.data.labels.push(t.getSeconds() + 's');
  chart.data.datasets[0].data.push(Math.round(value * 10) / 10);
  if (chart.data.labels.length > MAX_POINTS) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  chart.update('none');
}

// ─── Tab Navigation ───────────────────────────────────────
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => {
      b.classList.remove('text-emerald-400', 'border-emerald-500');
      b.classList.add('text-muted', 'border-transparent');
    });
    btn.classList.add('text-emerald-400', 'border-emerald-500');
    btn.classList.remove('text-muted', 'border-transparent');
    document.querySelectorAll('.tab-content').forEach(tc => tc.classList.add('hidden'));
    const tab = document.getElementById('tab-' + btn.dataset.tab);
    if (tab) tab.classList.remove('hidden');
  });
});

// ─── API fetch with timeout ───────────────────────────────
async function apiFetch(url) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 3000);
  try {
    const r = await fetch(url, { signal: ctrl.signal });
    clearTimeout(timer);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return await r.json();
  } catch (e) {
    clearTimeout(timer);
    throw e;
  }
}

// ─── Poll Status ──────────────────────────────────────────
async function fetchStatus() {
  try {
    const d = await apiFetch('/api/status');
    fetchFailCount = 0;
    if (JSON.stringify(d) !== JSON.stringify(lastStatus)) {
      lastStatus = d;
      updateDashboard(d);
    }
  } catch (e) {
    fetchFailCount++;
    const statusText = document.getElementById('statusText');
    const runLed = document.getElementById('runLed');
    const statusLed = document.getElementById('statusLed');
    if (fetchFailCount > 3) {
      statusText.textContent = 'OFFLINE';
      statusText.className = 'text-3xl sm:text-4xl font-bold tracking-tight text-red-400';
      runLed.className = 'w-5 h-5 rounded-full bg-red-500';
      statusLed.className = 'w-3 h-3 rounded-full bg-red-500';
    }
  }
}

// ─── Update Dashboard ─────────────────────────────────────
function updateDashboard(d) {
  const running = d.is_running;
  const sp = d.setpoint || 0;
  const rpm = d.current_rpm || 0;
  const error = sp - rpm;

  // Status text
  const st = document.getElementById('statusText');
  st.textContent = running ? 'RUN' : 'STOP';
  st.className = `text-3xl sm:text-4xl font-bold tracking-tight transition-colors duration-300 ${running ? 'text-emerald-400' : 'text-gray-500'}`;

  // LEDs
  const runLed = document.getElementById('runLed');
  runLed.className = `w-5 h-5 rounded-full transition-all duration-500 shadow-lg ${running ? 'bg-emerald-400 shadow-emerald-500/50 run-pulse' : 'bg-gray-600'}`;

  const statusLed = document.getElementById('statusLed');
  statusLed.className = `w-3 h-3 rounded-full transition-colors duration-500 ${running ? 'bg-emerald-400' : 'bg-gray-600'}`;

  // Metrics
  document.getElementById('setpointVal').textContent = sp.toFixed(1);
  document.getElementById('rpmVal').textContent = rpm.toFixed(1);
  const errEl = document.getElementById('errorVal');
  const errAbs = Math.abs(error);
  errEl.textContent = (error >= 0 ? '+' : '') + error.toFixed(1);
  errEl.className = `text-2xl sm:text-3xl font-bold transition-colors ${errAbs < 0.5 ? 'text-emerald-400' : errAbs < 2 ? 'text-amber-400' : 'text-red-400'}`;

  document.getElementById('pidOutVal').textContent = (d.pid_output || 0).toFixed(0);
  document.getElementById('vfdFreqVal').textContent = d.vfd_freq_raw || 0;

  // Modbus
  const mb = document.getElementById('modbusBadge');
  if (d.modbus_ok) {
    mb.textContent = 'Modbus: OK';
    mb.className = 'text-[10px] px-2 py-0.5 rounded-full font-medium bg-emerald-900/60 text-emerald-300 border border-emerald-800/50';
  } else {
    mb.textContent = 'Modbus: FAIL';
    mb.className = 'text-[10px] px-2 py-0.5 rounded-full font-medium bg-red-900/60 text-red-300 border border-red-800/50';
  }

  // Chart
  addChartPoint(rpm);

  // PID fields (only if not focused)
  ['pidKp', 'pidKi', 'pidKd'].forEach(id => {
    const el = document.getElementById(id);
    if (el && document.activeElement !== el) {
      const key = id.replace('pid', '').toLowerCase();
      el.value = (d[key] || 0).toFixed(id === 'pidKd' ? 1 : 2);
    }
  });

  // SP fields (only if not focused)
  document.querySelectorAll('.sp-input').forEach(inp => {
    if (document.activeElement !== inp) {
      const pos = inp.dataset.pos;
      const key = 'sp_pos' + pos;
      inp.value = (d[key] || 0).toFixed(1);
    }
  });

  // Uptime
  const ms = d.uptime_ms || 0;
  const sec = Math.floor(ms / 1000);
  const min = Math.floor(sec / 60);
  const hr = Math.floor(min / 60);
  document.getElementById('uptimeVal').textContent =
    hr > 0 ? `${hr}h ${min % 60}m` :
    min > 0 ? `${min}m ${sec % 60}s` :
    `${sec}s`;

  document.getElementById('heapVal').textContent = d.heap || '--';
  document.getElementById('chartStatus').textContent = `${Math.round(chart?.data?.labels?.length * POLL_MS / 1000 || 0)}s window`;

  // Buttons
  document.getElementById('btnStart').disabled = running;
  document.getElementById('btnStop').disabled = !running;

  // Offline recovery
  if (fetchFailCount > 0) fetchFailCount = 0;
}

// ─── Control Handlers ─────────────────────────────────────
async function sendCmd(url, okMsg, failMsg) {
  try {
    await apiFetch(url);
    toast(okMsg);
  } catch (e) {
    toast(failMsg || 'Request failed', false);
  }
}

document.getElementById('btnStart').addEventListener('click', () => {
  sendCmd('/api/start', 'Motor started', 'Start failed');
});

document.getElementById('btnStop').addEventListener('click', () => {
  sendCmd('/api/stop', 'Motor stopped', 'Stop failed');
});

document.getElementById('btnApplyPID').addEventListener('click', () => {
  const kp = document.getElementById('pidKp').value;
  const ki = document.getElementById('pidKi').value;
  const kd = document.getElementById('pidKd').value;
  sendCmd(`/api/pid?kp=${kp}&ki=${ki}&kd=${kd}`, 'PID updated', 'PID failed');
});

document.getElementById('btnSaveSP').addEventListener('click', () => {
  let count = 0;
  document.querySelectorAll('.sp-input').forEach(inp => {
    const pos = inp.dataset.pos;
    const val = inp.value;
    fetch(`/api/setpoint?pos=${pos}&value=${val}`);
    count++;
  });
  toast(`${count} setpoints saved`);
});

// ─── Init ─────────────────────────────────────────────────
initChart();
setInterval(fetchStatus, POLL_MS);
fetchStatus();
