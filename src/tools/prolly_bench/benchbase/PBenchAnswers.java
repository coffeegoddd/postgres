package com.oltpbenchmark.benchmarks.tpch.procedures;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HexFormat;

/** Opt-in, untimed correctness evidence. Ordinary benchmark runs use the original drain loop. */
public final class PBenchAnswers {
  private PBenchAnswers() {}

  private static String hash(byte[] bytes) {
    try {
      return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    } catch (NoSuchAlgorithmException e) {
      throw new AssertionError(e);
    }
  }

  private static void frame(DataOutputStream out, byte[] bytes) throws IOException {
    out.writeInt(bytes == null ? -1 : bytes.length);
    if (bytes != null) out.write(bytes);
  }

  public static synchronized void record(String query, String sql, ResultSet rs)
      throws SQLException {
    try {
      if (!query.matches("Q([1-9]|1[0-9]|2[0-2])")) {
        throw new SQLException("unexpected query identity: " + query);
      }
      ResultSetMetaData md = rs.getMetaData();
      ByteArrayOutputStream columns = new ByteArrayOutputStream();
      DataOutputStream schema = new DataOutputStream(columns);
      schema.writeInt(md.getColumnCount());
      for (int i = 1; i <= md.getColumnCount(); i++) {
        frame(schema, md.getColumnTypeName(i).getBytes(StandardCharsets.UTF_8));
        frame(schema, md.getColumnLabel(i).getBytes(StandardCharsets.UTF_8));
      }
      ArrayList<String> rows = new ArrayList<>();
      while (rs.next()) {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream row = new DataOutputStream(bytes);
        for (int i = 1; i <= md.getColumnCount(); i++) frame(row, rs.getBytes(i));
        rows.add(hash(bytes.toByteArray()));
      }
      String ordered = hash(String.join("", rows).getBytes(StandardCharsets.US_ASCII));
      Collections.sort(rows);
      String sorted = hash(String.join("", rows).getBytes(StandardCharsets.US_ASCII));
      // Every interpolated string is an allowlisted query name or hex digest.
      String line = "{\"version\":1,\"query\":\"" + query + "\",\"rows\":" + rows.size()
          + ",\"sql_sha256\":\"" + hash(sql.getBytes(StandardCharsets.UTF_8))
          + "\",\"columns_sha256\":\"" + hash(columns.toByteArray())
          + "\",\"ordered_sha256\":\"" + ordered + "\",\"sorted_sha256\":\"" + sorted + "\"}\n";
      Files.writeString(Path.of(System.getProperty("pbench.answers")), line,
          StandardOpenOption.CREATE, StandardOpenOption.APPEND);
    } catch (IOException e) {
      throw new SQLException("cannot preserve TPC-H correctness evidence", e);
    }
  }
}
