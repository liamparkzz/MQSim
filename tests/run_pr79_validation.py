"""Small deterministic WL-parameter and mapping-write integration checks."""
import argparse
import datetime
import hashlib
import itertools
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, default=Path('C:/CODEX/MQsimExperiments/20260909-pr79-integration'))
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
before = {str(f.relative_to(repo)): hashlib.sha256(f.read_bytes()).hexdigest() for f in (repo / 'src').rglob('*') if f.is_file()}
results = []
for scheduler, dynamic, static in itertools.product(['PRIORITY_OUT_OF_ORDER', 'OUT_OF_ORDER'], [False, True], [False, True]):
    case = f'{scheduler.lower()}-dynamic{int(dynamic)}-static{int(static)}'
    directory = a.output / 'cases' / case
    directory.mkdir(parents=True, exist_ok=True)
    trace = directory / 'input.trace'
    trace.write_text(''.join(f'{1 + i * 2000000} 0 {i * 16} 16 0\n' for i in range(128)), encoding='ascii')
    config = ET.parse(Path(__file__).parent / 'fixtures' / 'pr79-ssdconfig.xml')
    for tag, value in {'Transaction_Scheduling_Policy': scheduler, 'Dynamic_Wearleveling_Enabled': str(dynamic).lower(),
                       'Static_Wearleveling_Enabled': str(static).lower(), 'Static_Wearleveling_Threshold': '500'}.items():
        config.find('.//' + tag).text = value
    config.write(directory / 'ssdconfig.xml', encoding='us-ascii', xml_declaration=True)
    workload = ET.Element('MQSim_IO_Scenarios')
    flow = ET.SubElement(ET.SubElement(workload, 'IO_Scenario'), 'IO_Flow_Parameter_Set_Trace_Based')
    for tag, value in {'Priority_Class': 'HIGH', 'Device_Level_Data_Caching_Mode': 'TURNED_OFF', 'Channel_IDs': '0',
                       'Chip_IDs': '0', 'Die_IDs': '0', 'Plane_IDs': '0', 'Initial_Occupancy_Percentage': '0',
                       'File_Path': trace.as_posix(), 'Percentage_To_Be_Executed': '100', 'Relay_Count': '1', 'Time_Unit': 'NANOSECOND'}.items():
        ET.SubElement(flow, tag).text = value
    row = {'case': case, 'start': datetime.datetime.now().astimezone().isoformat()}
    outputs = {}
    for mode, exe in [('normal', 'MQSim.exe'), ('observer', 'MQSim-observer.exe')]:
        ET.ElementTree(workload).write(directory / f'{mode}.xml', encoding='us-ascii', xml_declaration=True)
        run = subprocess.run([str(a.output / exe), '-i', str(directory / 'ssdconfig.xml'), '-w', str(directory / f'{mode}.xml')],
                             cwd=directory, input=b'\n', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        output = run.stdout.decode('utf-8', errors='replace')
        (directory / f'{mode}.log').write_text(output, encoding='utf-8')
        (directory / f'{mode}.stderr.log').write_bytes(run.stderr)
        assert run.returncode == 0, (case, mode, run.returncode)
        counts = re.search(r'total requests generated: (\d+) total requests serviced:(\d+)', output)
        assert counts and counts.groups() == ('128', '128'), (case, mode, 'Host completion failure')
        outputs[mode] = output
    completion = re.search(r'OBS completion mapping_read=(\d+) mapping_write=(\d+)', outputs['observer'])
    queue = re.search(r'OBS queue residual=(\d+) ready=(\d+)', outputs['observer'])
    assert completion and completion.group(2) == '15', case
    assert queue and queue.groups() == ('0', '0'), case
    assert 'OBS settings_match=1' in outputs['observer'], case
    assert (directory / 'normal_scenario_1.xml').read_bytes() == (directory / 'observer_scenario_1.xml').read_bytes(), 'Observer perturbed results'
    row.update(host_completed=128, mapping_writes_completed=15, mapping_writes_residual=0, wl_parameters_match=True,
               observer_xml_identical=True, end=datetime.datetime.now().astimezone().isoformat())
    results.append(row)
    print('PASS ' + case)
    (a.output / 'summary.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
after = {str(f.relative_to(repo)): hashlib.sha256(f.read_bytes()).hexdigest() for f in (repo / 'src').rglob('*') if f.is_file()}
assert before == after, 'Simulator sources changed during validation'
(a.output / 'source-hashes.json').write_text(json.dumps(after, indent=2), encoding='utf-8')
print('PASS: 8 conditions / 16 processes; 149 source files unchanged during execution')
