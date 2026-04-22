for i in $(seq 1 30); do curl -s -o /dev/null -w "%{http_code} " http://localhost:8080/index.html; done
