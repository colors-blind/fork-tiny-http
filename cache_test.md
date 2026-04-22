# HTTP 缓存功能测试

## 编译

```bash
cd src
make clean && make
```

## 启动服务

```bash
cd src
./tiny 8080
```

## 核心概念

HTTP 缓存逻辑（`If-Modified-Since` + `Last-Modified`）：

- **服务器**：返回 `200` 时附带 `Last-Modified: <文件修改时间>`
- **客户端**：下次请求时带 `If-Modified-Since: <上次收到的时间>`
- **服务器判断**：
  - 如果 `文件修改时间 > If-Modified-Since` → 文件被修改过 → 返回 `200` + 新内容
  - 如果 `文件修改时间 <= If-Modified-Since` → 文件未修改 → 返回 `304`（无 body）

---

## 测试命令

### 测试 1：首次请求 - 验证 Last-Modified 响应头

```bash
curl -v http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 `200`
- 响应头包含 `Last-Modified: Wed, DD MMM YYYY HH:MM:SS GMT`
- 返回完整的 HTML 内容

**记录**：记下 `Last-Modified` 的值，比如 `Wed, 22 Apr 2026 10:30:00 GMT`

---

### 测试 2：用文件实际修改时间请求 - 验证 304

**注意**：把下面的 `If-Modified-Since` 值替换成测试 1 中实际返回的 `Last-Modified` 值。

```bash
curl -v -H "If-Modified-Since: Wed, 22 Apr 2026 10:30:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 `304 Not Modified`
- 响应头包含 `Last-Modified`
- **没有**响应体（没有 `Content-Length` 或 body 为空）

**原理**：`文件修改时间 == If-Modified-Since`，说明文件未被修改，返回 304。

---

### 测试 3：用过去时间请求（比文件修改时间早）- 验证 200

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2000 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 `200`
- 返回完整的 HTML 内容

**原理**：`文件修改时间(2026) > 2000-10-21`，说明文件在这个时间之后被修改过，返回 200。

---

### 测试 4：用未来时间请求 - 验证 304

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 `304 Not Modified`

**原理**：`文件修改时间(现在) <= 2026-10-21`，文件还没被修改，返回 304。

**说明**：这个场景不实用（浏览器不会发送未来时间），仅用于验证边界条件。

---

### 测试 5：HEAD 请求 + 缓存

```bash
curl -v -I -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 `304 Not Modified`
- 没有响应体

---

### 测试 6：404 页面（缓存不影响错误响应）

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/notfound.html
```

**预期结果：**
- 响应状态码 `404 Not Found`
- **不会**返回 304

---

### 测试 7：动态内容（CGI）不做缓存

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" "http://localhost:8080/cgi-bin/adder?1&2"
```

**预期结果：**
- 响应状态码 `200`
- 返回 CGI 执行结果
- **不会**返回 304（动态内容不做缓存处理）

---

## 测试计划

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 编译并启动服务 | 服务正常启动，无编译错误 |
| 2 | 运行测试 1（首次请求） | 200 + Last-Modified 头 |
| 3 | 运行测试 2（用测试 1 的 Last-Modified 值） | 304，无 body |
| 4 | 运行测试 3（过去时间） | 200，有 body |
| 5 | 运行测试 4（未来时间，可选） | 304 |
| 6 | 运行测试 5（HEAD + 缓存） | 304 |
| 7 | 运行测试 6（404） | 404，不返回 304 |
| 8 | 运行测试 7（CGI） | 200，不返回 304 |

---

## 边界情况说明

1. **If-Modified-Since 格式错误**：如果日期格式无法解析，服务器会忽略这个头，按正常流程返回 200。

2. **文件修改时间等于 If-Modified-Since**：`mtime <= ifModifiedSince` 条件成立，返回 304。这是标准行为。

3. **动态内容**：CGI 程序的输出不会做缓存检查，总是返回 200。

4. **错误响应**：404、403、500 等错误响应不会触发 304。

5. **POST 请求**：只对 GET 和 HEAD 请求做缓存处理，POST 不走缓存逻辑。

6. **Locale 问题**：代码已设置 `setlocale(LC_TIME, "C")`，确保 `strptime/strftime` 正确解析英文日期格式（Wed, Oct 等）。
