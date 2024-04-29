FROM gongjiang/ubt22-cuda121-py311

RUN pip install torch==2.1.2 torchvision==0.16.2 torchaudio==2.1.2 \
    sentencepiece numpy typing_extensions gguf transformers
