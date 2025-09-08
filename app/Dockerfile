# Dockerfile
FROM node:23-alpine

# Install required tools
RUN apk update && \
    apk add iputils-ping nmap build-base python3 git hiredis-dev cmake
    
WORKDIR /app
RUN git clone --depth 1 --branch 1.3.15 https://github.com/sewenew/redis-plus-plus.git && \
    cd redis-plus-plus && \
    mkdir build && \
    cd build && \
    cmake -DREDIS_PLUS_PLUS_BUILD_TEST=OFF -DREDIS_PLUS_PLUS_BUILD_SHARED=OFF .. && \
    make -j4 install && \
    cd /app && \
    rm -rf redis-plus-plus

COPY package*.json ./
RUN npm install

COPY . .
RUN npm run build

CMD node dist/worker.js & node dist/index.js
