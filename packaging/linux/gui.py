#!/usr/bin/env python3
"""Tk-based graphical builder for the DQIX Enhanced Linux AppImage.

The game itself is built locally from a ROM chosen by the user. No ROM or
ROM-generated code is part of this AppImage. Build output is kept in the
user-selected workspace.
"""
from __future__ import annotations

import fnmatch
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_NAME = "DQIX Enhanced — Constructeur Linux"
WORKSPACE_MARKER = ".dqix-enhanced-appimage-workspace"
IGNORE_NAMES = {
    ".git", "__pycache__", "build", "game", "extract", "obj", "gen", "gen7",
    "release", "rom", "reports", "session", "update", "bookmarks",
}
IGNORE_PATTERNS = (
    "*.nds", "*.sav", "*.o", "*.exe", "*.dll", "*.so", "*.ppm", "*.bmp",
    "*.wav", "*.pyc", "*.ico",
)


def source_ignore(directory: str, names: list[str]) -> set[str]:
    """Keep user build outputs/private data while refreshing packaged sources."""
    ignored: set[str] = set()
    for name in names:
        if name in IGNORE_NAMES or any(fnmatch.fnmatch(name, pat) for pat in IGNORE_PATTERNS):
            ignored.add(name)
    return ignored


def stage_from_output(line: str, current: int) -> tuple[int, str]:
    """Convert build-script stage messages into approximate, monotonic progress."""
    checks = (
        ("== Checking the ROM", 5, "Vérification de la ROM"),
        ("== Extracting the game's code", 12, "Extraction du code depuis la ROM"),
        ("extracted ARM9", 22, "Extraction terminée"),
        ("== Recompiling it to C", 28, "Recompilation ARM9/ARM7 vers C"),
        ("== Compiling", 55, "Compilation du runtime et du jeu"),
        ("== Putting the game folder together", 92, "Assemblage du dossier du jeu"),
        ("== Done.", 100, "Build terminé"),
    )
    for token, percent, status in checks:
        if token in line:
            return max(current, percent), status
    return current, ""


class BuilderApp:
    def __init__(self, root: tk.Tk, source: Path, default_workspace: Path, version: str) -> None:
        self.root = root
        self.source = source.resolve()
        self.default_workspace = default_workspace.expanduser()
        self.version = version
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None
        self.running = False
        self.progress_value = 0
        self.workspace_path: Path | None = None

        self.root.title(APP_NAME)
        self.root.geometry("850x650")
        self.root.minsize(720, 560)
        self._configure_style()

        outer = ttk.Frame(root, padding=20)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="DQIX Enhanced", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            outer,
            text="Construisez votre version Linux à partir de votre propre ROM. "
                 "La ROM et le jeu compilé restent sur votre ordinateur.",
            wraplength=790,
        ).pack(anchor="w", pady=(4, 18))

        form = ttk.Frame(outer)
        form.pack(fill="x")
        form.columnconfigure(1, weight=1)

        ttk.Label(form, text="Fichier ROM (.nds)").grid(row=0, column=0, sticky="w", padx=(0, 12), pady=7)
        self.rom_var = tk.StringVar()
        existing_rom = self.default_workspace / "game" / "data" / "dqix.nds"
        if existing_rom.is_file():
            self.rom_var.set(str(existing_rom))
        self.rom_entry = ttk.Entry(form, textvariable=self.rom_var)
        self.rom_entry.grid(row=0, column=1, sticky="ew", pady=7)
        self.rom_browse = ttk.Button(form, text="Parcourir…", command=self._choose_rom)
        self.rom_browse.grid(row=0, column=2, sticky="ew", padx=(10, 0), pady=7)

        ttk.Label(form, text="Dossier workspace").grid(row=1, column=0, sticky="w", padx=(0, 12), pady=7)
        self.workspace_var = tk.StringVar(value=str(self.default_workspace))
        self.workspace_entry = ttk.Entry(form, textvariable=self.workspace_var)
        self.workspace_entry.grid(row=1, column=1, sticky="ew", pady=7)
        self.workspace_browse = ttk.Button(form, text="Choisir…", command=self._choose_workspace)
        self.workspace_browse.grid(row=1, column=2, sticky="ew", padx=(10, 0), pady=7)

        ttk.Label(
            outer,
            text="Le workspace contient les sources copiées, les fichiers de build, le jeu et les sauvegardes. "
                 "Ne le publiez pas et ne le partagez pas.",
            wraplength=790,
            style="Hint.TLabel",
        ).pack(anchor="w", pady=(3, 15))

        actions = ttk.Frame(outer)
        actions.pack(fill="x", pady=(0, 12))
        self.build_button = ttk.Button(actions, text="Construire", command=self._start_build, style="Accent.TButton")
        self.build_button.pack(side="left")
        self.launch_button = ttk.Button(actions, text="Lancer le jeu", command=self._launch_game, state="disabled")
        self.launch_button.pack(side="left", padx=(10, 0))
        self.progress_text = tk.StringVar(value="Prêt — choisissez une ROM et un workspace.")
        ttk.Label(actions, textvariable=self.progress_text).pack(side="right", anchor="e")

        self.progress = ttk.Progressbar(outer, mode="determinate", maximum=100, value=0)
        self.progress.pack(fill="x", pady=(0, 12))

        ttk.Label(outer, text="Journal du build").pack(anchor="w")
        log_frame = ttk.Frame(outer)
        log_frame.pack(fill="both", expand=True, pady=(5, 0))
        self.log = tk.Text(
            log_frame, height=12, wrap="word", state="disabled",
            background="#111827", foreground="#e5e7eb",
            insertbackground="#e5e7eb", relief="flat", padx=10, pady=8,
        )
        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        footer = ttk.Frame(outer)
        footer.pack(fill="x", pady=(10, 0))
        ttk.Label(footer, text=f"Version source : {version}", style="Hint.TLabel").pack(side="left")
        ttk.Label(footer, text="Linux x86_64 · Toolchain incluse", style="Hint.TLabel").pack(side="right")

        self.root.after(100, self._poll_events)
        if existing_rom.is_file() and (self.default_workspace / "game" / "play.sh").is_file():
            self.launch_button.configure(state="normal")
            self.progress_text.set("Un build local existe déjà. Vous pouvez le relancer ou reconstruire.")

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("Title.TLabel", font=("TkDefaultFont", 19, "bold"))
        style.configure("Hint.TLabel", foreground="#596579")
        style.configure("Accent.TButton", padding=(16, 7))

    def _choose_rom(self) -> None:
        filename = filedialog.askopenfilename(
            parent=self.root,
            title="Sélectionner votre ROM Dragon Quest IX",
            filetypes=(("ROM Nintendo DS (*.nds)", "*.nds"), ("Tous les fichiers", "*")),
        )
        if filename:
            self.rom_var.set(filename)

    def _choose_workspace(self) -> None:
        current = Path(self.workspace_var.get()).expanduser()
        initial = current if current.is_dir() else current.parent
        if not initial.is_dir():
            initial = Path.home()
        directory = filedialog.askdirectory(
            parent=self.root,
            title="Choisir le dossier où créer le workspace",
            initialdir=str(initial),
            mustexist=True,
        )
        if directory:
            self.workspace_var.set(str(Path(directory).expanduser().resolve()))

    def _append_log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", text)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _start_build(self) -> None:
        if self.running:
            return
        rom_text = self.rom_var.get().strip()
        workspace_text = self.workspace_var.get().strip()
        if not rom_text:
            messagebox.showerror("ROM requise", "Sélectionnez d'abord votre fichier ROM .nds.", parent=self.root)
            return
        rom = Path(rom_text).expanduser()
        if not rom.is_file():
            messagebox.showerror("ROM introuvable", f"Le fichier indiqué n'existe pas :\n{rom}", parent=self.root)
            return
        if rom.suffix.lower() != ".nds":
            messagebox.showerror("Format invalide", "Le fichier sélectionné doit avoir l'extension .nds.", parent=self.root)
            return
        if not workspace_text:
            messagebox.showerror("Workspace requis", "Choisissez un dossier workspace.", parent=self.root)
            return
        workspace = Path(workspace_text).expanduser().resolve()
        if (workspace == Path.home() or workspace == Path("/") or workspace == self.source
                or workspace in self.source.parents or self.source in workspace.parents):
            messagebox.showerror(
                "Dossier non autorisé",
                "Choisissez un dossier dédié au workspace, et non votre dossier personnel, la racine ou les sources de l'AppImage.",
                parent=self.root,
            )
            return
        if workspace.exists() and not workspace.is_dir():
            messagebox.showerror("Dossier invalide", "Le workspace doit être un dossier.", parent=self.root)
            return
        marker = workspace / WORKSPACE_MARKER
        if workspace.exists() and any(workspace.iterdir()) and not marker.is_file():
            proceed = messagebox.askyesno(
                "Dossier non vide",
                "Ce dossier contient déjà des fichiers et n'a pas été initialisé par le constructeur. "
                "Les fichiers source portant le même nom pourront être remplacés ; les dossiers build/ et game/ "
                "seront préservés. Continuer ?",
                parent=self.root,
            )
            if not proceed:
                return

        self.running = True
        self.progress.stop()
        self.progress.configure(mode="determinate", value=0)
        self.progress_value = 1
        self.progress.configure(value=self.progress_value)
        self.progress_text.set("Préparation du workspace…")
        self.build_button.configure(state="disabled")
        self.launch_button.configure(state="disabled")
        self.rom_browse.configure(state="disabled")
        self.workspace_browse.configure(state="disabled")
        self._append_log(f"\n\n=== Build démarré ===\nROM : {rom}\nWorkspace : {workspace}\n")
        self.worker = threading.Thread(target=self._build_worker, args=(rom.resolve(), workspace), daemon=True)
        self.worker.start()

    def _build_worker(self, rom: Path, workspace: Path) -> None:
        worker_progress = 4
        try:
            self.events.put(("status", "Préparation des sources dans le workspace"))
            workspace.mkdir(parents=True, exist_ok=True)
            shutil.copytree(self.source, workspace, dirs_exist_ok=True, ignore=source_ignore)
            (workspace / WORKSPACE_MARKER).write_text(
                "Managed by DQIX Enhanced AppImage builder.\n"
                "Contains local build output and may contain a copy of a user-supplied ROM.\n",
                encoding="utf-8",
            )
            script = workspace / "tools" / "build-game.sh"
            if not script.is_file():
                raise RuntimeError(f"Le script de build est introuvable : {script}")
            self.events.put(("progress", (worker_progress, "Sources prêtes — vérification de la ROM")))
            process = subprocess.Popen(
                ["bash", str(script), str(rom)],
                cwd=str(workspace),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
            assert process.stdout is not None
            for line in process.stdout:
                self.events.put(("log", line))
                percent, status = stage_from_output(line, worker_progress)
                worker_progress = max(worker_progress, percent)
                if status:
                    self.events.put(("progress", (worker_progress, status)))
            code = process.wait()
            if code != 0:
                raise RuntimeError(f"Le script de build s'est terminé avec le code {code}. Consultez le journal ci-dessus.")
            self.events.put(("progress", (100, "Build terminé")))
            self.events.put(("success", workspace))
        except Exception as exc:  # surface failures inside the GUI instead of closing it
            self.events.put(("failure", str(exc)))

    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "status":
                    self.progress_text.set(str(payload))
                elif kind == "progress":
                    value, status = payload  # type: ignore[misc]
                    self.progress_value = max(self.progress_value, int(value))
                    status = str(status)
                    if status in ("Recompilation ARM9/ARM7 vers C", "Compilation du runtime et du jeu"):
                        if self.progress.cget("mode") != "indeterminate":
                            self.progress.stop()
                            self.progress.configure(mode="indeterminate")
                            self.progress.start(50)
                    else:
                        if self.progress.cget("mode") != "determinate":
                            self.progress.stop()
                            self.progress.configure(mode="determinate")
                        self.progress.configure(value=self.progress_value)
                    self.progress_text.set(status)
                elif kind == "success":
                    self.workspace_path = Path(payload)  # type: ignore[arg-type]
                    self.running = False
                    self.build_button.configure(state="normal")
                    self.rom_browse.configure(state="normal")
                    self.workspace_browse.configure(state="normal")
                    self.launch_button.configure(state="normal")
                    self._append_log("\n=== Build réussi ===\n")
                    messagebox.showinfo(
                        "Build terminé",
                        f"Le jeu a été construit avec succès.\n\nDossier du jeu :\n{self.workspace_path / 'game'}\n\n"
                        "Utilisez « Lancer le jeu » pour démarrer.",
                        parent=self.root,
                    )
                elif kind == "failure":
                    self.running = False
                    if self.progress.cget("mode") == "indeterminate":
                        self.progress.stop()
                        self.progress.configure(mode="determinate", value=self.progress_value)
                    self.build_button.configure(state="normal")
                    self.rom_browse.configure(state="normal")
                    self.workspace_browse.configure(state="normal")
                    self._append_log(f"\n=== ÉCHEC ===\n{payload}\n")
                    self.progress_text.set("Build en échec — consultez le journal.")
                    messagebox.showerror("Échec du build", str(payload), parent=self.root)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_events)

    def _launch_game(self) -> None:
        workspace = Path(self.workspace_var.get()).expanduser().resolve()
        play_script = workspace / "game" / "play.sh"
        if not play_script.is_file():
            messagebox.showerror("Jeu introuvable", "Aucun jeu compilé n'a été trouvé dans ce workspace.", parent=self.root)
            return
        try:
            subprocess.Popen(
                ["bash", str(play_script)],
                cwd=str(workspace / "game"),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
        except OSError as exc:
            messagebox.showerror("Impossible de lancer le jeu", str(exc), parent=self.root)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Interface graphique du constructeur Linux DQIX Enhanced")
    parser.add_argument("--source", required=True, help="racine source embarquée dans l'AppImage")
    parser.add_argument("--default-workspace", required=True, help="workspace initial proposé à l'utilisateur")
    parser.add_argument("--version", default="unknown")
    args = parser.parse_args()

    root = tk.Tk()
    BuilderApp(root, Path(args.source), Path(args.default_workspace), args.version)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
