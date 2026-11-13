# 在线目录

`read pkg://catalog` 默认读这里的 `index.json`（`https://raw.githubusercontent.com/hedykan/FeiyuOS/main/packages/index.json`），`write pkg://名字/install` 从这里下载安装。

- 每个文件就是一个程序或扩展包（开头的 `-- @…` 声明，见 [程序文档](../docs/PROGRAMS.zh-CN.md)），最大 16 KB。
- 改了任何 .lua 之后重新生成目录：`python3 tools/pkg_index.py packages`。目录记着每个文件的 SHA-256、大小和声明的权限，设备下载后逐项核对，对不上就不装。
- 更新：改 `@version`（数字比较，`1.10` 比 `1.9` 新），重新生成目录。设备上 `read pkg://catalog` 会显示「可更新」。
