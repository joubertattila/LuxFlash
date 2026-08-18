<?php
// Copy db_config.php.example to db_config.php (same folder) and fill in
// your own database credentials before deploying this page - see the
// comment in that file.
require __DIR__ . '/db_config.php';

// Set up paging (offset)
$offset = isset($_GET['offset']) ? max(0, intval($_GET['offset'])) : 0;
$limit = 100;
$step = 50;

$next_offset = $offset + $step;
$prev_offset = max(0, $offset - $step);

$conn = new mysqli($DB_HOST, $DB_USER, $DB_PASSWORD, $DB_NAME);
if ($conn->connect_error) die("Database error");

$sql = "SELECT * FROM light_measures ORDER BY timestamp DESC LIMIT $limit OFFSET $offset";
$result = $conn->query($sql);

$labels = []; $adc_data = []; $voltage_data = []; $cpu_temp_data = [];

if ($result && $result->num_rows > 0) {
    $rows = [];
    while ($row = $result->fetch_assoc()) $rows[] = $row;
    $rows = array_reverse($rows);

    foreach ($rows as $r) {
        $labels[] = date('H:i', strtotime($r['timestamp']));
        $adc_data[] = (int)$r['adc'];
        $voltage_data[] = (float)$r['voltage'];
        $cpu_temp_data[] = isset($r['cpu_temp_c']) ? (float)$r['cpu_temp_c'] : null;
    }
}

$last_adc = !empty($adc_data) ? end($adc_data) : null;
$last_voltage = !empty($voltage_data) ? end($voltage_data) : null;
$last_cpu_temp = !empty($cpu_temp_data) ? end($cpu_temp_data) : null;

$conn->close();
?>

<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>LightMeasure Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body { font-family: Arial; background:#eef2f3; text-align:center; }
.chart { background:white; margin:15px auto; padding:10px; border-radius:10px; width:90%; height:300px;}
.nav-buttons { margin: 15px; }
.nav-buttons button { padding: 10px 20px; margin: 0 10px; font-size: 16px; cursor: pointer; border-radius: 5px; border: 1px solid #ccc; background: #fff; transition: 0.2s;}
.nav-buttons button:hover { background: #e0e0e0; }
</style>
</head>
<body>

<h2>☀️ LightMeasure: Light Level</h2>
<h3><?php echo ($last_adc !== null) ? "Latest reading: ADC $last_adc / " . number_format($last_voltage, 3) . " V" : "No data"; ?></h3>

<h3><?php
// The CPU supply-voltage measurement (cpu_voltage) is DELIBERATELY not
// implemented on the Pico side (disabled for safety reasons on
// VSYS/GPIO29 - see LuxFlash/doc/LuxFlash_terv.md) - so that column is
// ALWAYS NULL, and we don't even query it here. This line depends ONLY
// on the temperature.
echo ($last_cpu_temp !== null) ? "CPU temperature: " . number_format($last_cpu_temp, 1) . " °C" : "";
?></h3>

<div class="nav-buttons">
    <a href="?offset=<?php echo $next_offset; ?>"><button>⏪ Older</button></a>
    <a href="?offset=<?php echo $prev_offset; ?>"><button <?php if ($offset <= 0) echo 'disabled'; ?>>Newer ⏩</button></a>
</div>

<div class="chart"><canvas id="light"></canvas></div>
<div class="chart"><canvas id="cpu"></canvas></div>

<script>
const labels = <?php echo json_encode($labels); ?>;
const adc_data = <?php echo json_encode($adc_data); ?>;
const voltage_data = <?php echo json_encode($voltage_data); ?>;
const cpu_temp_data = <?php echo json_encode($cpu_temp_data); ?>;

new Chart(document.getElementById("light"), {
    type: "line",
    data: {
        labels: labels,
        datasets: [
            { label: "ADC (raw)", data: adc_data, borderColor: "#f1c40f", yAxisID: 'y_adc', tension: 0.4 },
            { label: "Voltage (V)", data: voltage_data, borderColor: "#e74c3c", borderDash: [5,5], yAxisID: 'y_volt', tension: 0.4 }
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false,
        scales: {
            y_adc: { position: 'left', min: 0, max: 4096, title: { display: true, text: 'ADC (0-4096)' } },
            y_volt: { position: 'right', min: 0, max: 3.3, grid: { drawOnChartArea: false }, title: { display: true, text: 'Voltage (V)' } }
        }
    }
});

new Chart(document.getElementById("cpu"), {
    type: "line",
    data: {
        labels: labels,
        datasets: [
            { label: "CPU temp. (°C)", data: cpu_temp_data, borderColor: "#e67e22", borderDash: [5,5], yAxisID: 'y_cpu_temp', tension: 0.4 }
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false,
        scales: {
            y_cpu_temp: { position: 'right', min: 0, max: 150, grid: { drawOnChartArea: false }, title: { display: true, text: 'CPU temp. (°C)' } }
        }
    }
});
</script>
</body>
</html>
