FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    HF_HOME=/models \
    WHISPER_MODEL_DIR=/models \
    VAULT_DIR=/vault \
    DATA_DIR=/data

WORKDIR /srv
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY app ./app

EXPOSE 8080
HEALTHCHECK --interval=60s --timeout=5s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/health')"
CMD ["uvicorn", "--factory", "app.main:build", "--host", "0.0.0.0", "--port", "8080"]
