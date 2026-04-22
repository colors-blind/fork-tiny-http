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

## 测试命令

### 测试 1：首次请求 - 验证 Last-Modified 响应头

```bash
curl -v http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 200
- 响应头包含 `Last-Modified: <日期>`
- 返回完整的 HTML 内容

### 测试 2：带 If-Modified-Since 的请求（使用未来时间）- 验证 304

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 304 Not Modified
- 响应头包含 `Last-Modified: <日期>`
- **没有**响应体（Content-Length 不存在或为 0）

### 测试 3：带 If-Modified-Since 的请求（使用过去时间）- 验证 200

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2000 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 200
- 返回完整的 HTML 内容

### 测试 4：HEAD 请求 + 缓存

```bash
curl -v -I -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/index.html
```

**预期结果：**
- 响应状态码 304 Not Modified
- 没有响应体

### 测试 5：404 页面（缓存不影响错误响应）

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" http://localhost:8080/notfound.html
```

**预期结果：**
- 响应状态码 404 Not Found
- **不会**返回 304

### 测试 6：动态内容（CGI）不做缓存

```bash
curl -v -H "If-Modified-Since: Wed, 21 Oct 2026 07:28:00 GMT" "http://localhost:8080/cgi-bin/adder?1&2"
```

**预期结果：**
- 响应状态码 200
- 返回 CGI 执行结果
- **不会**返回 304（动态内容不做缓存处理）

---

## 测试计划

| 步骤 | 操作 | 预期结果 |
|------|------|----------|
| 1 | 编译并启动服务 | 服务正常启动，无编译错误 |
| 2 | 运行测试 1（首次请求） | 200 + Last-Modified 头 |
| 3 | 运行测试 2（未来时间） | 304，无 body |
| 4 | 运行测试 3（过去时间） | 200，有 body |
| 5 | 运行测试 4（HEAD + 缓存） | 304 |
| 6 | 运行测试 5（404） | 404，不返回 304 |
| 7 | 运行测试 6（CGI） | 200，不返回 304 |

---

## 边界情况说明

1. **If-Modified-Since 格式错误**：如果日期格式无法解析，服务器会忽略这个头，按正常流程返回 200。

2. **文件修改时间等于 If-Modified-Since**：`mtime <= ifModifiedSince` 条件成立，返回 304。这是标准行为。

3. **动态内容**：CGI 程序的输出不会做缓存检查，总是返回 200。

4. **错误响应**：404、403、500 等错误响应不会触发 304。

5. **POST 请求**：只对 GET 和 HEAD 请求做缓存处理，POST 不走缓存逻辑。
