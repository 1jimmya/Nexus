#!/usr/bin/env python3
"""Nexus 专用浏览器 — 原生 tkinter 窗口，地址栏 + 前进/后退/刷新/首页，
页面用 Nexus 语言写成（页面程序返回 {title, doc} 文档树），由本程序渲染。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk, messagebox

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES = os.path.join(HERE, "页面")
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "可执行程序", "nexus.exe")
if not os.path.isfile(EXE):
    EXE = os.path.join(ROOT, "可执行程序", "nexus.exe")

BG = "#eef3f8"
PANEL = "#ffffff"
INK = "#1d2b3a"
DIM = "#60758a"
LINE = "#d6e2ee"
ACCENT = "#1268b3"
SOFT = "#e3f0fb"


def resolve(url: str) -> str | None:
    """nexus://home / nexus://语法示例 / 本地 .nx 路径 -> 页面文件."""
    u = (url or "").strip()
    if u.startswith("nexus://"):
        name = u[len("nexus://"):].strip("/")
        if not name or name == "home":
            name = "首页"
        path = os.path.join(PAGES, name if name.endswith(".nx") else name + ".nx")
        return path if os.path.isfile(path) else None
    if u.startswith("file://"):
        u = u[7:]
    if os.path.isfile(u):
        return u
    path = os.path.join(PAGES, u if u.endswith(".nx") else u + ".nx")
    return path if os.path.isfile(path) else None


def eval_page(path: str):
    if not os.path.isfile(EXE):
        raise RuntimeError("找不到 可执行程序\\nexus.exe")
    p = subprocess.run([EXE, "run", path], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=30)
    if p.returncode != 0:
        raise RuntimeError((p.stdout or p.stderr or "页面执行失败").strip()[:500])
    data = json.loads(p.stdout)
    if not isinstance(data, dict):
        raise RuntimeError("页面必须返回记录")
    return data


class Browser:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Nexus 浏览器")
        self.root.geometry("1400x900")
        self.root.configure(bg=BG)
        self.history = []
        self.pos = -1
        self.current = None
        self._build()
        self.go("nexus://home")

    def _build(self):
        bar = tk.Frame(self.root, bg=PANEL, height=54)
        bar.pack(fill="x")
        bar.pack_propagate(False)
        for text, cmd in (("←", self.back), ("→", self.forward), ("⟳", self.refresh), ("⌂", self.home)):
            tk.Button(bar, text=text, command=cmd, bg=PANEL, fg=INK,
                      relief="flat", font=("Microsoft YaHei UI", 13), width=3).pack(side="left", padx=2)
        self.addr = tk.StringVar(value="nexus://home")
        entry = tk.Entry(bar, textvariable=self.addr, bg="#f7fbff", fg=INK,
                         relief="flat", font=("Consolas", 12))
        entry.pack(side="left", fill="x", expand=True, padx=8, pady=10)
        entry.bind("<Return>", lambda e: self.go(self.addr.get()))
        tk.Button(bar, text="前往", command=lambda: self.go(self.addr.get()),
                  bg=ACCENT, fg="white", relief="flat",
                  font=("Microsoft YaHei UI", 10), padx=16).pack(side="left", padx=6)

        self.canvas = tk.Canvas(self.root, bg=BG, highlightthickness=0)
        self.scroll = ttk.Scrollbar(self.root, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scroll.set)
        self.scroll.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner = tk.Frame(self.canvas, bg=BG)
        self.window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(self.window, width=e.width))

    def home(self):
        self.go("nexus://home")

    def back(self):
        if self.pos > 0:
            self.pos -= 1
            self._load(self.history[self.pos], push=False)

    def forward(self):
        if self.pos + 1 < len(self.history):
            self.pos += 1
            self._load(self.history[self.pos], push=False)

    def refresh(self):
        if self.current:
            self._load(self.current, push=False)

    def go(self, url):
        path = resolve(url)
        if not path:
            messagebox.showerror("Nexus 浏览器", "找不到页面：%s" % url)
            return
        self._load(path, push=True, url=url)

    def _load(self, path, push=True, url=None):
        self.addr.set(url or "nexus://" + os.path.splitext(os.path.basename(path))[0])
        try:
            data = eval_page(path)
        except Exception as exc:
            messagebox.showerror("Nexus 浏览器", str(exc))
            return
        self.current = path
        if push:
            self.history = self.history[:self.pos + 1]
            self.history.append(path)
            self.pos = len(self.history) - 1
        for w in self.inner.winfo_children():
            w.destroy()
        doc = data.get("doc") or data
        self._node(doc, self.inner, 0)
        self.root.title("%s · Nexus 浏览器" % (data.get("title") or "Nexus"))

    # ---- 文档树渲染 -------------------------------------------------
    def _node(self, node, parent, depth):
        if isinstance(node, list):
            for x in node:
                self._node(x, parent, depth)
            return
        if not isinstance(node, dict):
            return
        t = node.get("type")
        if t == "page":
            for c in node.get("children") or []:
                self._node(c, parent, depth)
        elif t == "heading":
            size = {1: 22, 2: 16, 3: 13}.get(node.get("level"), 14)
            tk.Label(parent, text=str(node.get("text", "")), bg=BG, fg=INK,
                     font=("Microsoft YaHei UI", size, "bold"), anchor="w",
                     justify="left").pack(fill="x", padx=24, pady=(12, 4))
        elif t == "text":
            tone = node.get("tone")
            color = {"dim": DIM, "ok": "#1f7a45", "warn": "#a86400", "bad": "#b42318"}.get(tone, INK)
            tk.Label(parent, text=str(node.get("text", "")), bg=BG, fg=color,
                     font=("Microsoft YaHei UI", 11), anchor="w", justify="left",
                     wraplength=1100).pack(fill="x", padx=24, pady=2)
        elif t == "card":
            card = tk.Frame(parent, bg=PANEL, bd=1, relief="solid")
            card.pack(fill="x", padx=24, pady=8)
            tk.Label(card, text=str(node.get("title", "")), bg=PANEL, fg=INK,
                     font=("Microsoft YaHei UI", 13, "bold"), anchor="w").pack(fill="x", padx=14, pady=(10, 2))
            body = tk.Frame(card, bg=PANEL)
            body.pack(fill="x", padx=14, pady=(0, 10))
            for c in node.get("children") or []:
                self._node(c, body, depth + 1)
        elif t == "row":
            row = tk.Frame(parent, bg=BG)
            row.pack(fill="x", padx=24, pady=4)
            for c in node.get("children") or []:
                self._node(c, row, depth + 1)
        elif t == "kv":
            for item in node.get("items") or []:
                if isinstance(item, list) and len(item) >= 2:
                    line = tk.Frame(parent, bg=BG)
                    line.pack(fill="x", padx=24, pady=1)
                    tk.Label(line, text=str(item[0]), bg=BG, fg=DIM,
                             font=("Microsoft YaHei UI", 10), width=14, anchor="w").pack(side="left")
                    tk.Label(line, text=str(item[1]), bg=BG, fg=INK,
                             font=("Microsoft YaHei UI", 10), anchor="w",
                             wraplength=900).pack(side="left", fill="x", expand=True)
        elif t == "table":
            headers = node.get("headers") or []
            rows = node.get("rows") or []
            table = tk.Frame(parent, bg=PANEL, bd=1, relief="solid")
            table.pack(fill="x", padx=24, pady=6)
            for j, h in enumerate(headers):
                tk.Label(table, text=str(h), bg=SOFT, fg=INK, font=("Microsoft YaHei UI", 10, "bold"),
                         bd=1, relief="solid", padx=6, pady=4).grid(row=0, column=j, sticky="nsew")
            for i, row in enumerate(rows):
                for j, cell in enumerate(row):
                    tk.Label(table, text=str(cell), bg=PANEL, fg=INK, font=("Microsoft YaHei UI", 10),
                             bd=1, relief="solid", padx=6, pady=3, anchor="w").grid(row=i + 1, column=j, sticky="nsew")
            for j in range(len(headers)):
                table.grid_columnconfigure(j, weight=1)
        elif t == "list":
            for item in node.get("items") or []:
                tk.Label(parent, text="• " + str(item), bg=BG, fg=INK,
                         font=("Microsoft YaHei UI", 10), anchor="w", justify="left",
                         wraplength=1100).pack(fill="x", padx=30, pady=1)
        elif t == "badge":
            tk.Label(parent, text=" " + str(node.get("text", "")) + " ", bg=SOFT, fg=ACCENT,
                     font=("Microsoft YaHei UI", 9), padx=6, pady=2).pack(side="left", padx=4)
        elif t == "button":
            tk.Button(parent, text=str(node.get("text", "")), bg=ACCENT, fg="white",
                      relief="flat", padx=12, pady=4,
                      command=lambda n=node: self._button(n)).pack(side="left", padx=6)
        elif t == "code":
            tk.Label(parent, text=str(node.get("text", "")), bg="#f4f7fb", fg=INK,
                     font=("Consolas", 10), anchor="w", justify="left",
                     wraplength=1050).pack(fill="x", padx=24, pady=4)
        elif t == "divider":
            tk.Frame(parent, bg=LINE, height=1).pack(fill="x", padx=24, pady=8)
        elif t == "spacer":
            tk.Frame(parent, bg=BG, height=int(node.get("size", 8))).pack(fill="x")
        elif t == "field":
            line = tk.Frame(parent, bg=BG)
            line.pack(fill="x", padx=24, pady=2)
            tk.Label(line, text=str(node.get("label", "")), bg=BG, fg=DIM, width=12, anchor="w",
                     font=("Microsoft YaHei UI", 10)).pack(side="left")
            tk.Entry(line, bg="white", fg=INK, relief="solid", bd=1,
                     font=("Microsoft YaHei UI", 10)).pack(side="left", fill="x", expand=True)

    def _button(self, node):
        target = node.get("goto") or node.get("action") or ""
        if isinstance(target, str) and target.startswith("goto:"):
            self.go(target[5:])
        elif target == "refresh":
            self.refresh()
        elif isinstance(target, str) and target.startswith("nexus://"):
            self.go(target)
        else:
            messagebox.showinfo("Nexus 浏览器", "按钮动作：%s" % target)

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    Browser().run()
