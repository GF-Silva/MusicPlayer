import requests
import time

def chunk_generator():
    # Vamos enviar pedaços maiores para testar a robustez
    for i in range(1, 6):
        data = f"Chunk numero {i} - Conteudo de stream longo...\n".encode('utf-8')
        yield data
        time.sleep(0.5)

def test_esp32_stream():
    url = "http://192.168.4.1/musics?nome=teste_stream.mp3"
    
    # Session mantém a conexão aberta e é mais estável para streaming
    session = requests.Session()
    
    try:
        # Forçamos o header de conexão para não fechar prematuramente
        headers = {'Connection': 'keep-alive'}
        
        print("Enviando stream...")
        response = session.put(url, data=chunk_generator(), headers=headers, timeout=30)
        
        print(f"Status Code: {response.status_code}")
        print(f"Resposta: {response.text}")
        
    except Exception as e:
        print(f"Erro: {e}")

if __name__ == "__main__":
    test_esp32_stream()
