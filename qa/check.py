from pathlib import Path
from tempfile import TemporaryDirectory
import json
import re
import subprocess


exe = Path(__file__).with_name("compare")


def run(old, new, out, kind, okay=True):
    result = subprocess.run([exe, old, new, out, kind], text=True, capture_output=True, timeout=10)
    assert (result.returncode == 0) == okay, result.stderr


def marked_lines(html):
    return re.findall(r"<span class=diff-line>(.*?)</span>", html)


with TemporaryDirectory() as directory:
    root = Path(directory)
    old, new = root / "PrivateOldRoot", root / "PrivateNewRoot"
    for folder in (old, new):
        (folder / "PrivateModule").mkdir(parents=True)

    old_source = (
        "/* PrivateHeader 中文 */\n#include <stdint.h>\n#define PrivateGlobal 10\n"
        "static int same(void)\n{\n    return 0;\n}\n"
        "static int value(void)\n{\n"
        '    const char *s = "<script> PrivateLiteral \\"quoted\\" 中文 }";\n'
        "    // PrivateComment }\\\n"
        "    PrivateContinuedComment\n"
        "    /* PrivateBlock\n       PrivateBlockTwo */\n"
        "    #if PrivateCondition\n"
        "    char marker = '\"';\n"
        "    #endif\n"
        "    return s[0]+1;\n}\n"
        "INTERRUPT(Timer2_ISR, INTERRUPT_TIMER2)\n{\n    count += 1;\n}\n"
        "void old_only(void) {}\n"
    )
    new_source = (old_source.replace("PrivateGlobal 10", "PrivateGlobal 16")
        .replace("s[0]+1", "s[0]+2").replace("count += 1", "count += 2")
        .replace("void old_only", "void new_only"))
    (old / "PrivateModule" / "PrivateMain.C").write_bytes(old_source.encode("gbk"))
    (new / "PrivateModule" / "PrivateMain.C").write_bytes(new_source.encode("utf-8-sig"))
    (old / "remove.c").write_text("char dropped(void) {return 0;}\n")
    (new / "add.c").write_text("char fresh(void) {return 0;}\n")
    for folder in (old, new):
        (folder / "common.h").write_text("#define SAME 17\n")
        (folder / "ignored.txt").write_text(str(folder))
        for i in range(40):
            (folder / f"z{i:02}.h").write_text(f"#define C{i:02} 0\n")
    (old / "z17.h").write_text("#define C17 1\n")
    report = root / "test.json"
    run(old, new, report, "json")
    result = json.loads(report.read_text())
    assert result["summary"] == {
        "files": 4, "added": 2, "modified": 2, "deleted": 2, "outsideChanges": 2,
        "comparedFiles": 44,
    }, result["summary"]
    assert result["redacted"] is True
    assert "oldRoot" not in result and "newRoot" not in result
    assert all(f["path"].startswith("File-") for f in result["files"])
    main = next(f for f in result["files"] if any(c["name"] == "Timer2_ISR" for c in f["functions"]))
    changes = {function["name"]: function for function in main["functions"]}
    assert set(changes) == {"value", "Timer2_ISR", "new_only", "old_only"}
    assert changes["Timer2_ISR"]["afterContent"] == (
        "INTERRUPT(Timer2_ISR, INTERRUPT_TIMER2)\n{\n    count += 2;\n}\n"
    )
    assert changes["value"]["beforeContent"].startswith("static int value(void)\n{")
    assert 'const char *s = "[REDACTED]";' in changes["value"]["afterContent"]
    assert "char marker = '\"';" in changes["value"]["afterContent"]
    assert changes["value"]["newStartLine"] == 8
    assert changes["new_only"]["beforeContent"] is None
    assert changes["old_only"]["afterContent"] is None
    assert main["outsideFunctionsChanged"] is True
    assert "outsideFunctions" not in main
    output = root / "test.html"
    run(old, new, output, "html")
    html = output.read_text()
    assert "[REDACTED]" in html and "<script>" not in html
    assert "函数外内容有变更" in html and "新增函数 2" in html
    assert sorted(marked_lines(html)) == sorted([
        "    return s[0]+1;", "    return s[0]+2;", "    count += 1;", "    count += 2;",
        "char dropped(void) {return 0;}", "char fresh(void) {return 0;}",
        "void old_only(void) {}", "void new_only(void) {}",
    ])
    for data in (html, report.read_text()):
        assert "Private" not in data and "中文" not in data

    run(old, new, report, "after")
    after_json = report.read_text()
    after = json.loads(after_json)
    assert after["redacted"] is True and after["mode"] == "afterOnly"
    assert after["summary"] == {"files": 2, "added": 2, "modified": 2, "comparedFiles": 44}
    expected = [
        {"path": file["path"], "functions": functions}
        for file in result["files"]
        if (functions := [{key: value for key, value in function.items() if key in
                           {"name", "changeType", "newStartLine", "newEndLine", "afterContent"}}
                          for function in file["functions"] if function["changeType"] != "deleted"])
    ]
    assert after["files"] == expected
    assert "Private" not in after_json and "中文" not in after_json
    run(old / "remove.c", new / "common.h", report, "after")
    assert not json.loads(report.read_text())["files"]

    run(old / "PrivateModule" / "PrivateMain.C", new / "PrivateModule" / "PrivateMain.C", report, "json")
    assert json.loads(report.read_text())["summary"]["modified"] == 2
    run(old, old, report, "json")
    assert not json.loads(report.read_text())["files"]
    run(old, new / "add.c", report, "json", okay=False)
    run(old / "remove.c", new / "add.c", old / "remove.c", "json", okay=False)
    assert (old / "remove.c").read_text().startswith("char dropped")
    a, b = root / "same.c", root / "same.h"
    a.write_bytes(b"int normalized(void)\r\n{ return 1; }\r\n")
    b.write_text(a.read_text())
    run(a, b, report, "json")
    assert not json.loads(report.read_text())["files"]
    before = (
        'void lines(void) {\n    int x = 1;\n    keep();\n    erase();\n'
        '    keep();\n    finish();\n}\n'
        'void label(void) {\n    print("PrivateOld");\n}\n'
    )
    after = (before.replace("x = 1", "x = 2")
             .replace("    erase();\n", "")
             .replace("    keep();", "\tkeep ( ); /* PrivateInline */")
             .replace("    finish();", "    compare(x < 5 && x > 0);\n\n    finish();")
             .replace("PrivateOld", "PrivateNew"))
    a.write_text(before)
    b.write_text(after)
    run(a, b, output, "html")
    html = output.read_text()
    assert marked_lines(html) == [
        "    int x = 1;", "    erase();", "    int x = 2;",
        "    compare(x &lt; 5 &amp;&amp; x &gt; 0);",
        '    print("[REDACTED]");', '    print("[REDACTED]");',
    ]
    assert "标红行的变更内容已脱敏。" in html and "Private" not in html

    a.write_text(
        '/* Old file header */\n#define VALUE (x+1)  // change me\n'
        'int same(void) {\n    return value(2+3); /* body comment */\n}\n'
    )
    b.write_text(
        '// New file header\n\n # define  VALUE\t( x + 1 )\n'
        '/* Added comment before function */\n'
        'int same ( void )\n{\n\n    /* Added block\n       comment */\n'
        '\treturn value(2\n    + 3);\n\n}\n// Added footer\n'
    )
    run(a, b, report, "json")
    ignored = json.loads(report.read_text())
    assert not ignored["files"] and ignored["summary"]["outsideChanges"] == 0
    run(a, b, output, "html")
    assert "<article>" not in output.read_text() and not marked_lines(output.read_text())

    a.write_text(
        '#define ADD(x) x + 1\n'
        'int changed(void) {\n'
        '    stable(8 + 9);\n'
        '    f(x);\n'
        '    return 1;\n'
        '}\n'
        'int operators(int x) {return x++ + 2;}\n'
    )
    b.write_text(
        '#define ADD (x) x + 1\n'
        'int changed(void) {\n'
        '    stable(8\n         + 9); /* Only wrapped and commented */\n'
        '    f(z);\n'
        '    f(x);\n'
        '    return 1 + 2;\n'
        '}\n'
        'int operators(int x) {return x + ++2;}\n'
    )
    run(a, b, report, "json")
    significant = json.loads(report.read_text())
    assert significant["summary"]["modified"] == 2
    assert significant["summary"]["outsideChanges"] == 1
    run(a, b, output, "html")
    assert marked_lines(output.read_text()) == [
        "    return 1;", "    f(z);", "    return 1 + 2;",
        "int operators(int x) {return x++ + 2;}",
        "int operators(int x) {return x + ++2;}",
    ]

    a.write_text('int conditional(void) {\n#if 1 /* PrivateHeader\nPrivateOld */\n'
                 '    return 0;\n#endif\n}\n')
    b.write_text(a.read_text().replace("#if 1", "#if 2").replace("PrivateOld", "PrivateNew"))
    run(a, b, output, "html")
    html = output.read_text()
    assert marked_lines(html) == ["/* 预处理指令已省略 */"] * 2
    run(a, b, report, "json")
    assert "Private" not in html + report.read_text()

print("Passed recursive and single-file comparisons, function additions/deletions, macro ISR, "
      "GBK/UTF-8, insignificant changes, HTML highlighting, both JSON modes, redaction and source-overwrite guard.")
