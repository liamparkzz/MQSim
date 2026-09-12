"""Small desktop front end; all conversion rules live in mqsim_trace_converter.py."""
from pathlib import Path
import queue
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import mqsim_trace_converter as converter

PROFILES = {
    "RocksDB — overwritezipf": "rocksdb-overwritezipf",
    "RocksDB — YCSB-A": "rocksdb-ycsba",
    "Mobile — CSV / CSV.GZ": "mobile",
    "MSRC_1 — CSV / CSV.GZ": "msrc1",
    "MSRC_2 — CSV / CSV.GZ": "msrc2",
    "CloudPhysics — 원본 VSCSI": "cloudphysics-vscsi",
    "CloudPhysics — CSV": "cloudphysics-csv",
}


def build_app(root):
    root.title("MQSim 공용 트레이스 변환기 " + converter.VERSION)
    root.geometry("880x700")
    root.minsize(820, 640)
    panel = ttk.Frame(root, padding=18)
    panel.pack(fill="both", expand=True)
    panel.columnconfigure(1, weight=1)
    panel.rowconfigure(10, weight=1)
    profile = tk.StringVar(value=next(iter(PROFILES)))
    output_dir, capacity, device = tk.StringVar(), tk.StringVar(), tk.StringVar()
    length_unit = tk.StringVar(value="선택 필요")
    subnanosecond = tk.StringVar(value="error")
    sort_time = tk.BooleanVar(value=False)
    files, events = [], queue.Queue()
    status = tk.StringVar(value="입력 파일과 형식을 선택하세요. 원본 파일은 변경하지 않습니다.")
    running = False

    ttk.Label(panel, text="입력 형식").grid(row=0, column=0, sticky="w", pady=5)
    ttk.Combobox(panel, textvariable=profile, values=list(PROFILES), state="readonly").grid(row=0, column=1, columnspan=2, sticky="ew")
    listing = tk.Listbox(panel, height=5)
    listing.grid(row=2, column=0, columnspan=3, sticky="ew", pady=6)

    def choose_files():
        chosen = filedialog.askopenfilenames(title="같은 형식의 트레이스 파일 선택")
        if chosen:
            files[:] = chosen
            listing.delete(0, "end")
            for path in files:
                listing.insert("end", path)
            if not output_dir.get():
                output_dir.set(str(Path(files[0]).parent / "converted"))

    ttk.Button(panel, text="입력 파일 선택 (여러 개 가능)", command=choose_files).grid(row=1, column=0, columnspan=3, sticky="w", pady=8)
    ttk.Label(panel, text="결과 폴더").grid(row=3, column=0, sticky="w")
    ttk.Entry(panel, textvariable=output_dir).grid(row=3, column=1, sticky="ew")

    def choose_output():
        chosen = filedialog.askdirectory(title="결과 폴더")
        if chosen:
            output_dir.set(chosen)

    ttk.Button(panel, text="찾기", command=choose_output).grid(row=3, column=2, padx=(6, 0))
    ttk.Label(panel, text="논리 용량 검사 (선택)").grid(row=4, column=0, sticky="w", pady=6)
    ttk.Entry(panel, textvariable=capacity).grid(row=4, column=1, sticky="ew")
    ttk.Label(panel, text="예: 256GB / 256GiB").grid(row=4, column=2)
    ttk.Label(panel, text="원본 디스크 선택 (선택)").grid(row=5, column=0, sticky="w", pady=6)
    ttk.Entry(panel, textvariable=device).grid(row=5, column=1, sticky="ew")
    ttk.Label(panel, text="MSRC: usr:0 / Mobile: 8388608").grid(row=5, column=2)
    ttk.Label(panel, text="CloudPhysics CSV의 len 단위").grid(row=6, column=0, sticky="w", pady=6)
    ttk.Combobox(panel, textvariable=length_unit, values=["선택 필요", "bytes", "sectors"], state="readonly").grid(row=6, column=1, sticky="ew")
    ttk.Checkbutton(panel, text="시간 순서로 정렬 — 같은 시각의 요청은 원본 순서 유지 (디스크 공간 추가 사용)", variable=sort_time).grid(row=7, column=0, columnspan=3, sticky="w", pady=10)
    ttk.Label(panel, text="1ns 미만 시간값 처리").grid(row=8, column=0, sticky="w", pady=6)
    ttk.Combobox(panel, textvariable=subnanosecond, values=["error", "floor", "nearest"], state="readonly").grid(row=8, column=1, sticky="ew")
    ttk.Label(panel, text="오류 / 내림 / 반올림").grid(row=8, column=2)
    log = tk.Text(panel, height=10, wrap="word", state="disabled")
    log.grid(row=10, column=0, columnspan=3, sticky="nsew", pady=8)
    ttk.Label(panel, textvariable=status, wraplength=800).grid(row=11, column=0, columnspan=3, sticky="w")

    def start():
        nonlocal running
        if not files or not output_dir.get():
            messagebox.showerror("입력 확인", "입력 파일과 결과 폴더를 선택하세요.")
            return
        fmt = PROFILES[profile.get()]
        opts = dict(capacity=capacity.get().strip() or None, source_device=device.get().strip() or None,
                    sort_time=sort_time.get(), length_unit=length_unit.get() if fmt == "cloudphysics-csv" else None,
                    subnanosecond=subnanosecond.get())
        try:
            converter.parse_capacity(opts["capacity"])
            if fmt == "cloudphysics-csv" and opts["length_unit"] not in ("bytes", "sectors"):
                raise ValueError("CloudPhysics CSV의 len 단위를 선택하세요.")
        except ValueError as error:
            messagebox.showerror("입력 확인", str(error))
            return
        selected = list(files)
        folder = Path(output_dir.get())
        names = [Path(path).name + ".trace" for path in selected]
        if len({name.casefold() for name in names}) != len(names):
            messagebox.showerror("입력 확인", "이름이 같은 파일은 별도 결과 폴더로 변환하세요.")
            return
        running = True
        start_button.configure(state="disabled")
        status.set("변환 중입니다. 기존 결과 파일은 덮어쓰지 않습니다.")

        def worker():
            failures = 0
            for path, name in zip(selected, names):
                destination = folder / name
                try:
                    events.put(("log", f"시작: {path}"))
                    result = converter.convert(path, destination, fmt, **opts,
                                               progress=lambda n: events.put(("status", f"{name}: {n:,}개 요청 처리 중")))
                    counts = result["output"]
                    events.put(("log", f"완료: {destination}\n요청 {counts['requests']:,} / 읽기 {counts['reads']:,} / 쓰기 {counts['writes']:,}\n검증 기록: {destination}.manifest.json"))
                except Exception as error:
                    failures += 1
                    events.put(("log", f"실패: {path}\n{error}"))
            events.put(("done", f"전체 {len(selected)}개 중 {len(selected)-failures}개 완료, {failures}개 실패"))

        threading.Thread(target=worker, daemon=False).start()

    start_button = ttk.Button(panel, text="변환 및 검증 시작", command=start)
    start_button.grid(row=9, column=0, columnspan=3, sticky="ew")

    def poll():
        nonlocal running
        while True:
            try:
                kind, message = events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                log.configure(state="normal")
                log.insert("end", message + "\n\n")
                log.see("end")
                log.configure(state="disabled")
            else:
                status.set(message)
            if kind == "done":
                running = False
                start_button.configure(state="normal")
        root.after(150, poll)

    def close():
        if running:
            messagebox.showinfo("변환 중", "파일 검증을 마친 뒤 창을 닫아 주세요.")
        else:
            root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    root.after(150, poll)
    return panel


if __name__ == "__main__":
    window = tk.Tk()
    build_app(window)
    window.mainloop()
