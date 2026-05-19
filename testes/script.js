const http = require('http');

const server = http.createServer((req, res) => {
  if (req.url === '/stream') {
    res.writeHead(200, {
      'Content-Type': 'text/plain',
      'Transfer-Encoding': 'chunked' 
    });

    let count = 0;
    const interval = setInterval(() => {
      res.write(`Chunk ${count}\n`); // Envia um pedaço
      count++;

      if (count === 10) {
        clearInterval(interval);
        res.end('Fim do stream.\n'); // Envia o chunk final (0)
      }
    }, 1000); // 1 segundo de intervalo entre cada envio
  } else {
    res.writeHead(404);
    res.end();
  }
});

server.listen(3000, () => {
  console.log('Servidor rodando em http://localhost:3000/stream');
});
