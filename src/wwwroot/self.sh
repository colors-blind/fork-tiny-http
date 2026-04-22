# 1. 首次请求，获取 Last-Modified 时间
echo "=== 首次请求 ==="
curl -v http://localhost:8080/index.html 2>&1 | tee headers.txt

# 2. 从响应头中提取 Last-Modified 时间
LAST_MODIFIED=$(grep -i "< last-modified" headers.txt | sed 's/< last-modified: //I' | tr -d '\r')
echo "Last-Modified: $LAST_MODIFIED"

# 3. 使用相同的时间发送条件请求（应该返回304）
echo -e "\n=== 条件请求（期望304） ==="
curl -v -H "If-Modified-Since: $LAST_MODIFIED" http://localhost:8080/index.html
