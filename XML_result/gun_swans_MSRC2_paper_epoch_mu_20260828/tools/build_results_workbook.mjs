import fs from "node:fs/promises";
import fsSync from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const bundle = path.resolve(process.argv[2]);
const workspace = path.resolve(bundle, "..", "..");
const dataDir = path.join(bundle, "workbook_data");
const excelDir = path.join(bundle, "results", "excel");
const previewDir = path.join(bundle, "previews");
const codexOutputDir = path.join(workspace, "outputs", "01a04221-62a4-7ea3-aa78-de4dc5281add");
const fileName = "gun_swans_MSRC2_paper_epoch_mu_results.xlsx";
const bundleOutput = path.join(excelDir, fileName);
const codexOutput = path.join(codexOutputDir, fileName);

await fs.mkdir(excelDir, { recursive: true });
await fs.mkdir(previewDir, { recursive: true });
await fs.mkdir(codexOutputDir, { recursive: true });

function csvRows(text) {
  const rows = [];
  let row = [];
  let field = "";
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quoted) {
      if (ch === '"' && text[i + 1] === '"') {
        field += '"';
        i++;
      } else if (ch === '"') {
        quoted = false;
      } else {
        field += ch;
      }
    } else if (ch === '"') {
      quoted = true;
    } else if (ch === ',') {
      row.push(field);
      field = "";
    } else if (ch === '\n') {
      row.push(field.replace(/\r$/, ""));
      if (row.some((value) => value !== "")) rows.push(row);
      row = [];
      field = "";
    } else {
      field += ch;
    }
  }
  if (field || row.length) {
    row.push(field.replace(/\r$/, ""));
    rows.push(row);
  }
  if (rows.length && rows[0].length) rows[0][0] = rows[0][0].replace(/^\uFEFF/, "");
  return rows;
}

function csvEscape(value) {
  const text = value === null || value === undefined ? "" : String(value);
  return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
}

function toCsv(rows) {
  return rows.map((row) => row.map(csvEscape).join(",")).join("\r\n") + "\r\n";
}

function colLetter(index) {
  let value = index;
  let result = "";
  while (value > 0) {
    value--;
    result = String.fromCharCode(65 + (value % 26)) + result;
    value = Math.floor(value / 26);
  }
  return result;
}

function lineCount(text) {
  let count = 0;
  for (let i = 0; i < text.length; i++) if (text.charCodeAt(i) === 10) count++;
  return count + (text.length && !text.endsWith("\n") ? 1 : 0);
}

function lastTag(text, tag, fallback = "") {
  const expression = new RegExp(`<${tag}>([^<]*)</${tag}>`, "g");
  let value = fallback;
  for (const match of text.matchAll(expression)) value = match[1].trim();
  return value;
}

async function sha256(filePath) {
  return await new Promise((resolve, reject) => {
    const hash = crypto.createHash("sha256");
    const input = fsSync.createReadStream(filePath);
    input.on("data", (chunk) => hash.update(chunk));
    input.on("error", reject);
    input.on("end", () => resolve(hash.digest("hex").toUpperCase()));
  });
}

async function walk(directory) {
  const output = [];
  for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
    const full = path.join(directory, entry.name);
    const rel = path.relative(bundle, full).replaceAll("\\", "/");
    if (entry.name === "node_modules") continue;
    if (entry.isDirectory()) {
      if (["work", "workbook_data", "previews"].includes(entry.name)) continue;
      if (rel === "results/excel") continue;
      output.push(...await walk(full));
    } else if (!entry.name.toLowerCase().endsWith(".xlsx")) {
      output.push(full);
    }
  }
  return output;
}

function categoryFor(relativePath) {
  if (relativePath.startsWith("source/")) return "Source snapshot";
  if (relativePath.startsWith("bin/")) return "Executable";
  if (relativePath.startsWith("inputs/")) return "Input XML";
  if (relativePath.startsWith("results/raw_xml/")) return "Result XML";
  if (relativePath.startsWith("manifest/")) return "Manifest";
  if (relativePath.startsWith("tools/")) return "Reproduction tool";
  if (relativePath.startsWith("logs/")) return "Execution log";
  return "Bundle file";
}

let summaryCsv = await fs.readFile(path.join(dataDir, "summary.csv"), "utf8");
let epochCsv = await fs.readFile(path.join(dataDir, "epoch_mu_excel.csv"), "utf8");
let perSsdCsv = await fs.readFile(path.join(dataDir, "per_ssd.csv"), "utf8");
let migrationCsv = await fs.readFile(path.join(dataDir, "migrations.csv"), "utf8");
let manifestCsv = await fs.readFile(path.join(bundle, "manifest", "trace_manifest.csv"), "utf8");
const configText = await fs.readFile(path.join(bundle, "inputs", "ssdconfig_gun_swans_paper_epochlog.xml"), "utf8");
const summaryParsed = csvRows(summaryCsv);
const summaryDataRows = summaryParsed.slice(1);
const perSsdParsed = csvRows(perSsdCsv);
const migrationParsed = csvRows(migrationCsv);
const manifestParsed = csvRows(manifestCsv);
const rowCounts = {
  summary: lineCount(summaryCsv),
  epoch: lineCount(epochCsv),
  perSsd: lineCount(perSsdCsv),
  migration: lineCount(migrationCsv),
  manifest: lineCount(manifestCsv),
};
const epochLastRow = rowCounts.epoch;

const sourceRows = [["Category", "File", "Relative_Path", "Bytes", "Modified_Local", "SHA256"]];
for (const filePath of await walk(bundle)) {
  const relativePath = path.relative(bundle, filePath).replaceAll("\\", "/");
  const stat = await fs.stat(filePath);
  sourceRows.push([
    categoryFor(relativePath),
    path.basename(filePath),
    relativePath,
    stat.size,
    stat.mtime.toISOString(),
    await sha256(filePath),
  ]);
}
sourceRows.splice(1, sourceRows.length - 1, ...sourceRows.slice(1).sort((a, b) => a[0].localeCompare(b[0]) || a[2].localeCompare(b[2])));
const workbook = await Workbook.fromCSV(epochCsv, { sheetName: "Epoch Mu" });
const summarySheet = workbook.worksheets.add("Summary");
const parametersSheet = workbook.worksheets.add("Parameters");
const eolAuditSheet = workbook.worksheets.add("EOL Audit");
const readmeSheet = workbook.worksheets.add("README");

function addRowsSheet(name, rows) {
  const sheet = workbook.worksheets.add(name);
  if (rows.length && rows[0].length) {
    sheet.getRange(`A1:${colLetter(rows[0].length)}${rows.length}`).values = rows;
  }
  return sheet;
}

addRowsSheet("Raw Summary", summaryParsed);
addRowsSheet("Per SSD", perSsdParsed);
addRowsSheet("Migration Log", migrationParsed);
addRowsSheet("Trace Manifest", manifestParsed);
addRowsSheet("Source Files", sourceRows);

summaryCsv = null;
epochCsv = null;
perSsdCsv = null;
migrationCsv = null;
manifestCsv = null;

const colors = {
  navy: "#17324D",
  teal: "#0E7490",
  tealLight: "#CCFBF1",
  blueLight: "#E8F1F5",
  amberLight: "#FEF3C7",
  redLight: "#FEE2E2",
  greenLight: "#DCFCE7",
  grayLight: "#F3F4F6",
  border: "#CBD5E1",
  white: "#FFFFFF",
  text: "#1F2937",
};

function title(sheet, address, text) {
  const range = sheet.getRange(address);
  range.merge();
  range.values = [[text]];
  range.format = {
    fill: colors.navy,
    font: { bold: true, color: colors.white, size: 15 },
    horizontalAlignment: "left",
    verticalAlignment: "center",
  };
  range.format.rowHeight = 32;
}

function header(range) {
  range.format = {
    fill: colors.teal,
    font: { bold: true, color: colors.white },
    horizontalAlignment: "center",
    verticalAlignment: "center",
    wrapText: true,
    borders: { preset: "outside", style: "thin", color: colors.border },
  };
  range.format.rowHeight = 34;
}

function body(range) {
  range.format = {
    font: { color: colors.text },
    verticalAlignment: "center",
    borders: { insideHorizontal: { style: "thin", color: "#E5E7EB" } },
  };
}

for (const sheet of [summarySheet, parametersSheet, eolAuditSheet, readmeSheet]) sheet.showGridLines = false;

// Summary: visible decision sheet with formulas linked to raw sheets.
const summaryHeaders = [
  "Trace", "Requests", "Write %", "Avg latency (us)", "IOPS", "Max Mu (pp)",
  "Avg Mu (pp)", "Epochs Mu>=5", "Epochs Mu>=15", "Normal epochs", "Redirect epochs",
  "Migration epochs", "Redirect ops", "Migration ops", "Migration occurred", "EOL triggered",
  "First EOL SSD", "EOL time (ns)", "Min effective OP", "Bad blocks", "Max erase",
  "Total erases", "Flash WAF", "Sim seconds", "Status", "Source CSV", "Source revision",
];
title(summarySheet, "A1:AA1", "gun_swans — MSRC_2 Completed Trace Results (standardized format + epoch Mu logs)");
summarySheet.getRange("A2:AA2").merge();
summarySheet.getRange("A2:AA2").values = [[
  `${summaryDataRows.length} successfully completed MSRC_2 traces; remaining traces are deferred. Original timing (1x); 40 s epoch; 16 MiB zones; thresholds 5/15; OP 20%; device cache off. Mu is the percentage-point standard deviation of cumulative per-SSD physical write share.`,
]];
summarySheet.getRange("A2:AA2").format = { fill: colors.blueLight, font: { color: colors.text }, wrapText: true };
summarySheet.getRange("A4:AA4").values = [summaryHeaders];
header(summarySheet.getRange("A4:AA4"));
const firstSummaryRow = 5;
const lastSummaryRow = firstSummaryRow + summaryDataRows.length - 1;
if (summaryDataRows.length) {
  summarySheet.getRange(`A${firstSummaryRow}:AA${firstSummaryRow}`).formulas = [[
    "='Raw Summary'!A2", "='Raw Summary'!F2", "=IF(B5=0,0,'Raw Summary'!H2/B5)",
    "='Raw Summary'!O2", "='Raw Summary'!I2",
    `=IFERROR(MAXIFS('Epoch Mu'!$D$2:$D$${epochLastRow},'Epoch Mu'!$A$2:$A$${epochLastRow},$A5),0)`,
    `=IFERROR(AVERAGEIFS('Epoch Mu'!$D$2:$D$${epochLastRow},'Epoch Mu'!$A$2:$A$${epochLastRow},$A5),0)`,
    `=COUNTIFS('Epoch Mu'!$A$2:$A$${epochLastRow},$A5,'Epoch Mu'!$D$2:$D$${epochLastRow},\">=5\")`,
    `=COUNTIFS('Epoch Mu'!$A$2:$A$${epochLastRow},$A5,'Epoch Mu'!$D$2:$D$${epochLastRow},\">=15\")`,
    "='Raw Summary'!T2", "='Raw Summary'!U2", "='Raw Summary'!V2",
    "='Raw Summary'!W2", "='Raw Summary'!X2", "=IF(N5>0,\"YES\",\"NO\")",
    "='Raw Summary'!AK2", "='Raw Summary'!AL2", "='Raw Summary'!AM2",
    "='Raw Summary'!AQ2", "='Raw Summary'!AP2", "='Raw Summary'!AR2",
    "='Raw Summary'!AS2", "='Raw Summary'!AD2", "='Raw Summary'!AT2",
    "='Raw Summary'!B2", "='Raw Summary'!C2", "='Raw Summary'!AW2",
  ]];
  summarySheet.getRange(`A${firstSummaryRow}:AA${lastSummaryRow}`).fillDown();
  body(summarySheet.getRange(`A${firstSummaryRow}:AA${lastSummaryRow}`));
  summarySheet.getRange(`B${firstSummaryRow}:B${lastSummaryRow}`).format.numberFormat = "#,##0";
  summarySheet.getRange(`C${firstSummaryRow}:C${lastSummaryRow}`).format.numberFormat = "0.00%";
  summarySheet.getRange(`D${firstSummaryRow}:G${lastSummaryRow}`).format.numberFormat = "#,##0.000";
  summarySheet.getRange(`H${firstSummaryRow}:N${lastSummaryRow}`).format.numberFormat = "#,##0";
  summarySheet.getRange(`Q${firstSummaryRow}:R${lastSummaryRow}`).format.numberFormat = "#,##0";
  summarySheet.getRange(`S${firstSummaryRow}:S${lastSummaryRow}`).format.numberFormat = "0.0000%";
  summarySheet.getRange(`T${firstSummaryRow}:V${lastSummaryRow}`).format.numberFormat = "#,##0";
  summarySheet.getRange(`W${firstSummaryRow}:X${lastSummaryRow}`).format.numberFormat = "#,##0.000";
}
summarySheet.freezePanes.freezeRows(4);
for (const [column, width] of Object.entries({ A: 30, B: 14, C: 11, D: 16, E: 14, F: 14, G: 14, H: 14, I: 14, J: 14, K: 15, L: 16, M: 13, N: 13, O: 16, P: 14, Q: 14, R: 19, S: 16, T: 13, U: 12, V: 14, W: 12, X: 13, Y: 12, Z: 64, AA: 36 })) {
  summarySheet.getRange(`${column}1:${column}${Math.max(lastSummaryRow, 5)}`).format.columnWidth = width;
}

// Parameters and exact provenance of the policy values.
title(parametersSheet, "A1:E1", "Experiment Parameters and Definitions");
parametersSheet.getRange("A3:E3").values = [["Category", "Parameter", "Value", "Unit", "Notes"]];
header(parametersSheet.getRange("A3:E3"));
const parameterRows = [
  ["Code", "Source code name", "gun_swans", "", "Instrumented source snapshot is bundled under source/gun_swans"],
  ["Code", "Executable", "MQSim_gun_swans_paper_epochlog.exe", "", "Built from the bundled snapshot"],
  ["SWANS paper", "SSD_Count", Number(lastTag(configText, "SSD_Count")), "SSDs", "Four-member array"],
  ["SWANS paper", "Stripe_Unit_LBA", Number(lastTag(configText, "Stripe_Unit_LBA")), "512-B sectors", "RAID stripe unit"],
  ["SWANS paper", "SWANS_Zone_Size_LBA", Number(lastTag(configText, "SWANS_Zone_Size_LBA")), "512-B sectors", "16 MiB zone"],
  ["Derived", "SWANS zone size", null, "MiB", "Formula: LBA × 512 B"],
  ["SWANS paper", "SWANS_Epoch_Default", Number(lastTag(configText, "SWANS_Epoch_Default")), "ns", "Policy evaluation interval"],
  ["Derived", "SWANS epoch", null, "seconds", "Formula: ns / 1e9"],
  ["SWANS paper", "TH precautionary", Number(lastTag(configText, "SWANS_TH_Precautionary")), "percentage points", "Mu >= 5 enters redirect band"],
  ["SWANS paper", "TH critical", Number(lastTag(configText, "SWANS_TH_Critical")), "percentage points", "Mu >= 15 attempts migration"],
  ["SWANS paper", "Max concurrent migrations", Number(lastTag(configText, "SWANS_Max_Concurrent_Migrations")), "tasks", "Ping-pong control"],
  ["Lab control", "Overprovisioning_Ratio", Number(lastTag(configText, "Overprovisioning_Ratio")), "ratio", "20% OP per user requirement"],
  ["Lab control", "Device cache", "TURNED_OFF", "", "Trace workload setting"],
  ["Lab control", "Buffered write completion", lastTag(configText, "SWANS_Buffered_Write_Completion_Mode"), "", "Writes wait with migration reads; no early completion"],
  ["Lab control", "Initial occupancy", 70, "%", "Applied to every trace"],
  ["Endurance", "Block PE cycle limit", Number(lastTag(configText, "Block_PE_Cycles_Limit")), "erases", "Block is retired at the limit"],
  ["Replay", "Time acceleration", 1, "x", "Original MSRC timing preserved"],
  ["Replay", "Address mode", "GLOBAL_LBA", "", "Original source LBA retained"],
  ["Replay", "Percentage / Relay", "100 / 1", "% / count", "Every valid request once"],
  ["Logging", "Epoch Mu unit", "percentage points", "pp", "Std. dev. of cumulative physical-write shares"],
];
parametersSheet.getRange(`A4:E${parameterRows.length + 3}`).values = parameterRows;
parametersSheet.getRange("C9").formulas = [["=C8*512/1024/1024"]];
parametersSheet.getRange("C11").formulas = [["=C10/1000000000"]];
body(parametersSheet.getRange(`A4:E${parameterRows.length + 3}`));
parametersSheet.getRange(`A4:A${parameterRows.length + 3}`).format = { fill: colors.grayLight, font: { bold: true, color: colors.text } };
parametersSheet.getRange(`E4:E${parameterRows.length + 3}`).format.wrapText = true;
parametersSheet.getRange("C15").format.numberFormat = "0.0%";
parametersSheet.freezePanes.freezeRows(3);
for (const [column, width] of Object.entries({ A: 18, B: 34, C: 32, D: 20, E: 72 })) parametersSheet.getRange(`${column}1:${column}${parameterRows.length + 3}`).format.columnWidth = width;

// Audit answers question 1 directly and documents exact new semantics.
title(eolAuditSheet, "A1:F1", "Audit — Values Requested in the Reference Image");
eolAuditSheet.getRange("A2:F2").merge();
eolAuditSheet.getRange("A2:F2").values = [["Conclusion: the previous XML/Excel did not contain all nine values. The new gun_swans XML and this workbook cover all nine, including EOL-time snapshots."]];
eolAuditSheet.getRange("A2:F2").format = { fill: colors.amberLight, font: { bold: true, color: colors.text }, wrapText: true };
eolAuditSheet.getRange("A4:F4").values = [["Requested field", "Previous XML", "Previous Excel", "New XML", "Scope in new Summary", "Definition"]];
header(eolAuditSheet.getRange("A4:F4"));
const auditRows = [
  ["EOL_Triggered", "Equivalent only (OP_Exhausted_Any_SSD)", "Partial", "YES", "Array", "True when any SSD consumes its OP reserve"],
  ["First_EOL_SSD_ID", "NO", "NO", "YES", "Array", "SSD with earliest OP exhaustion time; -1 means none"],
  ["EOL_Time", "Per-SSD equivalent only", "Partial", "YES", "Array + per SSD", "Simulation time in ns of first OP exhaustion"],
  ["EOL_Bad_Block_Count", "NO", "NO", "YES", "First EOL SSD", "Retired bad-block count snapshotted at EOL"],
  ["EOL_Effective_OP_Ratio", "NO", "NO", "YES", "First EOL SSD", "Effective OP ratio snapshotted at EOL"],
  ["Bad_Block_Count", "YES (per SSD)", "YES (limited)", "YES", "Sum across SSDs + per SSD", "Currently retired blocks"],
  ["Current_Effective_OP_Ratio", "NO", "NO", "YES", "Minimum across SSDs + per SSD", "max(OP reserve - retired - pending, 0) / total blocks"],
  ["Max_Block_Erase_Count", "YES (per SSD)", "NO", "YES", "Maximum across SSDs + per SSD", "Largest block erase count"],
  ["Total_Block_Erase_Count", "NO (page-erase proxy only)", "NO", "YES", "Sum across SSDs + per SSD", "Exact sum of every block's erase count"],
];
eolAuditSheet.getRange(`A5:F${auditRows.length + 4}`).values = auditRows;
body(eolAuditSheet.getRange(`A5:F${auditRows.length + 4}`));
eolAuditSheet.getRange(`A5:A${auditRows.length + 4}`).format = { fill: colors.grayLight, font: { bold: true, color: colors.text } };
eolAuditSheet.getRange(`B5:F${auditRows.length + 4}`).format.wrapText = true;
eolAuditSheet.freezePanes.freezeRows(4);
for (const [column, width] of Object.entries({ A: 34, B: 36, C: 20, D: 13, E: 34, F: 72 })) eolAuditSheet.getRange(`${column}1:${column}${auditRows.length + 4}`).format.columnWidth = width;

// README / interpretation guide.
title(readmeSheet, "A1:B1", "README — Scope, Reproduction, and Interpretation");
readmeSheet.getRange("A3:B3").values = [["Item", "Description"]];
header(readmeSheet.getRange("A3:B3"));
const readmeRows = [
  ["Scope", `${summaryDataRows.length} successfully completed result XML files. Failed/deferred traces have no result row; their state remains in Trace Manifest. Duplicate/compact variants are intentionally retained.`],
  ["Code identity", "The exact source snapshot, compiled executable, configuration, generated workload XML files, run logs, and manifests are in this same bundle."],
  ["Mu log", `Epoch Mu contains ${Math.max(rowCounts.epoch - 1, 0).toLocaleString("en-US")} raw epoch rows. Mu is the population standard deviation of cumulative per-SSD physical-write shares, in percentage points.`],
  ["Thresholds", "Mu < 5: NORMAL; 5 <= Mu < 15: REDIRECT; Mu >= 15: MIGRATION is attempted. A migration epoch may fall back to REDIRECT when no valid hot/cold zone pair exists."],
  ["Write accounting", "Migration restore writes are added to cumulative physical writes (WAM). SSD-by-SSD host and migration totals remain separately available in Per SSD and raw XML."],
  ["No cache", "Device-level trace caching is TURNED_OFF. Buffered writes wait for the migration read/write path and are not acknowledged early."],
  ["EOL stop", "At OP exhaustion the simulator stops immediately. EOL snapshot fields therefore describe the actual first exhaustion event, not a later steady state."],
  ["Effective OP", "The ratio accounts for both retired blocks and retirement operations already pending when the threshold is reached."],
  ["Timing", "MSRC FILETIME timestamps are normalized to the first request and replayed at 1x in nanoseconds. Each trace is an independent simulation."],
  ["Address mapping", "GLOBAL_LBA keeps the source LBA. MQSim maps the array-global LBA through gun_swans RAID/SWANS placement."],
  ["Trace provenance", "Trace Manifest contains source path, byte size, SHA-256, converter counts, timings, and completion status. Source Files hashes every bundled source/input/result/tool file."],
  ["Raw preservation", "Raw Summary, Per SSD, Migration Log, Epoch Mu, and the raw XML directory retain values used by the visible Summary sheet."],
  ["Formula policy", "Summary Write %, Max/Average Mu, threshold counts, and migration occurrence are Excel formulas linked to the raw sheets."],
];
readmeSheet.getRange(`A4:B${readmeRows.length + 3}`).values = readmeRows;
body(readmeSheet.getRange(`A4:B${readmeRows.length + 3}`));
readmeSheet.getRange(`A4:A${readmeRows.length + 3}`).format = { fill: colors.grayLight, font: { bold: true, color: colors.text } };
readmeSheet.getRange(`B4:B${readmeRows.length + 3}`).format.wrapText = true;
readmeSheet.freezePanes.freezeRows(3);
readmeSheet.getRange(`A1:A${readmeRows.length + 3}`).format.columnWidth = 24;
readmeSheet.getRange(`B1:B${readmeRows.length + 3}`).format.columnWidth = 110;

function styleImported(sheetName, rows, columns, widths = {}) {
  const sheet = workbook.worksheets.getItem(sheetName);
  const lastCol = colLetter(columns);
  header(sheet.getRange(`A1:${lastCol}1`));
  sheet.freezePanes.freezeRows(1);
  for (const [column, width] of Object.entries(widths)) sheet.getRange(`${column}1:${column}${Math.max(rows, 2)}`).format.columnWidth = width;
  return sheet;
}

const rawSummarySheet = styleImported("Raw Summary", rowCounts.summary, 50, { A: 30, B: 12, C: 64, D: 18, E: 68, F: 14, O: 18, AG: 14, AK: 14, AU: 64, AW: 36, AX: 68 });
rawSummarySheet.getRange(`D2:D${rowCounts.summary}`).format.numberFormat = "#,##0";
rawSummarySheet.getRange(`F2:I${rowCounts.summary}`).format.numberFormat = "#,##0.000";
rawSummarySheet.getRange(`AO2:AQ${rowCounts.summary}`).format.numberFormat = "0.0000%";

const epochSheet = styleImported("Epoch Mu", rowCounts.epoch, 4, { A: 30, B: 12, C: 20, D: 13 });
if (rowCounts.epoch > 1) {
  epochSheet.getRange(`B2:C${rowCounts.epoch}`).format.numberFormat = "#,##0";
  epochSheet.getRange(`D2:D${rowCounts.epoch}`).format.numberFormat = "0.000000";
}

const perSsdSheet = styleImported("Per SSD", rowCounts.perSsd, 30, { A: 30, B: 9, C: 17, D: 17, F: 18, J: 20, P: 18, Q: 14, V: 13, W: 14, AD: 18 });
if (rowCounts.perSsd > 1) perSsdSheet.getRange(`P2:P${rowCounts.perSsd}`).format.numberFormat = "0.0000%";

styleImported("Migration Log", rowCounts.migration, 15, { A: 30, B: 11, C: 20, D: 10, E: 10, F: 18, G: 18, N: 14, O: 16 });
styleImported("Trace Manifest", rowCounts.manifest, 27, { A: 32, B: 12, C: 36, D: 68, E: 68, F: 18, G: 68, H: 17, I: 17, X: 68, AA: 60 });
const sourceSheet = styleImported("Source Files", sourceRows.length, 6, { A: 20, B: 42, C: 76, D: 18, E: 25, F: 68 });
sourceSheet.getRange(`D2:D${sourceRows.length}`).format.numberFormat = "#,##0";

const summaryInspect = await workbook.inspect({ kind: "table", sheetId: "Summary", range: `A1:AA${Math.min(lastSummaryRow, 12)}`, maxChars: 20000 });
console.log(summaryInspect.ndjson);
const epochInspect = await workbook.inspect({ kind: "table", sheetId: "Epoch Mu", range: "A1:D8", maxChars: 12000 });
console.log(epochInspect.ndjson);
const formulaErrors = await workbook.inspect({
  kind: "match",
  searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
  options: { useRegex: true, maxResults: 300 },
  summary: "pre-export formula error scan",
});
console.log(formulaErrors.ndjson);

for (const [sheetName, range, fileName] of [
  ["Summary", `A1:AA${Math.min(lastSummaryRow, 15)}`, "01_summary.png"],
  ["Parameters", `A1:E${parameterRows.length + 3}`, "02_parameters.png"],
  ["EOL Audit", `A1:F${auditRows.length + 4}`, "03_eol_audit.png"],
  ["Epoch Mu", "A1:D20", "04_epoch_mu.png"],
  ["README", `A1:B${readmeRows.length + 3}`, "05_readme.png"],
]) {
  const image = await workbook.render({ sheetName, range, scale: 1, format: "png" });
  await fs.writeFile(path.join(previewDir, fileName), new Uint8Array(await image.arrayBuffer()));
}

const output = await SpreadsheetFile.exportXlsx(workbook);
await output.save(bundleOutput);
await fs.copyFile(bundleOutput, codexOutput);
console.log(JSON.stringify({
  bundleOutput,
  codexOutput,
  traceCount: summaryDataRows.length,
  epochRows: Math.max(rowCounts.epoch - 1, 0),
  sourceFiles: sourceRows.length - 1,
  previewDir,
}));
