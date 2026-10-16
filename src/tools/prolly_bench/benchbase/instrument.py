"""Apply a narrow, checked instrumentation hook to the copied BenchBase source."""
import shutil
import sys
from pathlib import Path

root = Path(sys.argv[1]) / "src/main/java/com/oltpbenchmark/benchmarks/tpch/procedures"
path = root / "GenericQuery.java"
source = path.read_text()
original = """        while (rs.next()) {
          // do nothing
        }"""
replacement = """        if (System.getProperty("pbench.answers") != null) {
          PBenchAnswers.record(getClass().getSimpleName(), stmt.toString(), rs);
        } else {
          while (rs.next()) {
            // do nothing
          }
        }"""
if source.count(original) != 1:
    raise SystemExit("BenchBase GenericQuery changed; review the correctness hook before building")
path.write_text(source.replace(original, replacement))
shutil.copy2(Path(__file__).with_name("PBenchAnswers.java"), root / "PBenchAnswers.java")
