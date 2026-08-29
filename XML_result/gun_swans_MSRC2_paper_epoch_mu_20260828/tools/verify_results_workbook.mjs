import fs from "node:fs/promises";
import path from "node:path";
import { FileBlob, SpreadsheetFile } from "@oai/artifact-tool";

const workbookPath = path.resolve(process.argv[2]);
const epochCsvPath = path.resolve(process.argv[3]);
const epochCsv = await fs.readFile(epochCsvPath, "utf8");
let epochRows = 0;
for (let i = 0; i < epochCsv.length; i++) if (epochCsv.charCodeAt(i) === 10) epochRows++;
epochRows = Math.max(epochRows - 1, 0);

const workbook = await SpreadsheetFile.importXlsx(await FileBlob.load(workbookPath));
const sheets = await workbook.inspect({ kind: "sheet", include: "id,name", maxChars: 12000 });
const expected = [
  "Summary", "Parameters", "EOL Audit", "README", "Raw Summary", "Epoch Mu",
  "Per SSD", "Migration Log", "Trace Manifest", "Source Files",
];
for (const name of expected) {
  if (!(sheets.ndjson ?? "").includes(name)) throw new Error(`Missing sheet: ${name}`);
}

const summary = await workbook.inspect({ kind: "table", sheetId: "Summary", range: "A1:AA29", maxChars: 50000 });
const formulas = await workbook.inspect({
  kind: "formula",
  sheetId: "Summary",
  range: "A5:AA29",
  maxChars: 30000,
  options: { maxResults: 1000 },
});
const audit = await workbook.inspect({ kind: "table", sheetId: "EOL Audit", range: "A1:F13", maxChars: 30000 });
const epochHead = await workbook.inspect({ kind: "table", sheetId: "Epoch Mu", range: "A1:P5", maxChars: 12000 });
const epochTail = await workbook.inspect({
  kind: "table",
  sheetId: "Epoch Mu",
  range: `A${epochRows + 1}:P${epochRows + 1}`,
  maxChars: 8000,
});
const errors = await workbook.inspect({
  kind: "match",
  searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
  options: { useRegex: true, maxResults: 500 },
  summary: "post-export formula error scan",
});

for (const field of [
  "EOL_Triggered", "First_EOL_SSD_ID", "EOL_Time", "EOL_Bad_Block_Count",
  "EOL_Effective_OP_Ratio", "Bad_Block_Count", "Current_Effective_OP_Ratio",
  "Max_Block_Erase_Count", "Total_Block_Erase_Count",
]) {
  if (!(audit.ndjson ?? "").includes(field)) throw new Error(`EOL audit missing ${field}`);
}
if (!(formulas.ndjson ?? "").includes("MAXIFS")) throw new Error("Summary MAXIFS formula missing");
if (!(errors.ndjson ?? "").includes("matched 0")) throw new Error(`Formula error scan failed: ${errors.ndjson}`);
if (!(epochTail.ndjson ?? "").includes("Trace_Name") && (epochTail.ndjson ?? "").length < 50) {
  throw new Error("Epoch tail row could not be inspected");
}

console.log(sheets.ndjson);
console.log(summary.ndjson);
console.log(formulas.ndjson);
console.log(audit.ndjson);
console.log(epochHead.ndjson);
console.log(epochTail.ndjson);
console.log(errors.ndjson);
console.log(JSON.stringify({ workbookPath, epochRows, verification: "passed" }));
