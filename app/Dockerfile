# Dockerfile
FROM node:23-alpine

# Install required tools
RUN apk update && \
    apk add iputils-ping nmap

WORKDIR /app
COPY package*.json ./
RUN npm install
COPY . .
RUN npm run build

CMD node dist/worker.js & node dist/index.js
