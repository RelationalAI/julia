FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive
RUN apt update && apt upgrade -y
RUN apt-get install -y build-essential libatomic1 python3 python3-pip gfortran perl wget m4 cmake pkg-config git
RUN apt-get install -y tree curl wget gcc gdb

CMD ["/bin/bash"]
