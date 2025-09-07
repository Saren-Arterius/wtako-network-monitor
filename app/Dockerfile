# Dockerfile
FROM node:23-alpine

# Install required tools
RUN apk update && \
    apk add iputils-ping nmap build-base python3
    
WORKDIR /app
COPY package*.json ./
RUN npm install
COPY . .
WORKDIR /app
RUN npm run build

CMD node dist/worker.js & node dist/index.js
