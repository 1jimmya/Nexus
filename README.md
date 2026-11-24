# Nexus 语言（开源版 1.1.0）

Nexus 是一门前缀节点语言。本目录是可直接双击运行、也可上传开源的语言发行包。

## 双击运行

- **Nexus解释器.exe**：交互式解释器；
- **启动浏览器.exe**：Nexus 专用浏览器（地址栏 + `.nx` 页面渲染）；
- 编译 C 引擎：运行 `编译脚本\编译C引擎.ps1`。

## 目录

```
Nexus语言/
  Nexus解释器.exe       双击进入交互解释器
  启动浏览器.exe        双击打开专用浏览器
  LICENSE               0BSD 许可证
  可执行程序/nexus.exe    单文件 C 引擎
  解释器源代码/nexus.c    C11 解释器源码
  浏览器/                原生浏览器 + 页面
  示例/                  8 个 .nx 示例
  编译脚本/              编译 C 引擎
  说明文档/              语法手册、使用说明、完整规范、别名总表、许可证
  README.md
  常用命令.ps1
```

## 交互

```
nexus> (+ 1 2)
3
nexus> :help
nexus> :quit
```

纯求值模式：

```powershell
.\可执行程序\nexus.exe eval --pure "(+ 1 2)"
```

## 文档

- `说明文档\语法手册.md`
- `说明文档\使用说明.md`
- `说明文档\完整语言规范.md`
- `说明文档\别名总表.md`

## 许可证

0BSD，不要求署名；见 `LICENSE` 与 `说明文档\许可证.txt`。

## 开源上传

```powershell
git remote add origin <仓库地址>
git push -u origin main
```
