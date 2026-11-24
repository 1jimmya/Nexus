# Nexus 语言（开源版 1.1.0）

Nexus 是一门**前缀节点语言**：代码与数据同形，一切都是 `(head arg …)`，没有运算符优先级，嵌套就是优先级。
本目录是可直接双击运行、也可直接上传开源的语言发行包。

## 双击运行

- **Nexus解释器.exe**：打开交互式 Nexus 解释器；
- **启动浏览器.exe**：打开 Nexus 专用浏览器（地址栏 + 前进/后退/刷新/首页，渲染 `.nx` 页面）；
- 编译 C 引擎：右键“使用 PowerShell 运行”`编译脚本\编译C引擎.ps1`。

## 目录

```
Nexus语言/
  Nexus解释器.exe       双击进入交互解释器
  启动浏览器.exe        双击打开 Nexus 专用浏览器
  LICENSE               0BSD 许可证（标准文件）
  可执行程序/
    nexus.exe           单文件 C 引擎（无宿主，双击即 REPL）
  解释器源代码/
    nexus.c             C11 解释器源码
  浏览器/
    Nexus浏览器.py      原生 tkinter 浏览器（地址栏 + 文档树渲染）
    页面/               首页.nx、语法示例.nx
    启动器.c            启动浏览器.exe 的源码
  示例/                 8 个 .nx 示例
  编译脚本/
    编译C引擎.ps1       编译脚本（MSVC / gcc / clang）
  说明文档/
    语法手册.md
    使用说明.md
    更新记录.md
    许可证.txt          0BSD 中文副本
  README.md
  常用命令.ps1
```

## 交互使用

双击 `Nexus解释器.exe`，或运行：

```powershell
.\可执行程序\nexus.exe
```

```
nexus> (+ 1 2)
3
nexus> (node square (x) (* x x))
null
nexus> (square 9)
81
nexus> :help
nexus> :quit
```

REPL 支持多行表达式、`:help`、`:load <文件>`、`:quit`。

## 专用浏览器

双击 `启动浏览器.exe`，窗口上方是地址栏与前进/后退/刷新/首页；页面不是 HTML，
而是 `浏览器\页面\*.nx` 返回的文档树，由浏览器渲染。默认地址 `nexus://home`，
也可输入 `nexus://语法示例` 或本地 `.nx` 路径。

## 命令行

```powershell
.\可执行程序\nexus.exe run 示例\hello.nx
.\可执行程序\nexus.exe ast 示例\hello.nx
.\可执行程序\nexus.exe tokens 示例\hello.nx
.\可执行程序\nexus.exe test
.\可执行程序\nexus.exe version
```

## 许可证

**0BSD**（BSD Zero Clause License）：允许使用、复制、修改、分发、商用，**不要求署名**；
只保留免责声明。标准文件：`LICENSE`；中文副本：`说明文档\许可证.txt`。

## 开源上传

```powershell
git remote add origin <你的仓库地址>
git push -u origin main
```

发布包：`..\Nexus语言-1.1.0.zip`（含源码、exe、浏览器、文档、示例）。
