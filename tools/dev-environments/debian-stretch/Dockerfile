#
# Use with a shared network stack with the host to connect
# to a postgresql db on the host without exposing the postgresql
# port to the network. This is not secure, but fine for 
# development purposes.
#
# docker run --rm -v code_dir:/devel:rw -i -t --net=host my_image
#
FROM debian:stretch

RUN apt update && \
    apt -y install libpoco-dev libpocofoundation46 libpoconet46 libpocoutil46 \
            libgdal20 libgdal-dev cmake g++ build-essential \
            libpq-dev discount python bash vim && \
    mkdir /devel

EXPOSE 9090 9091
VOLUME ["/devel"]

CMD ["/bin/bash"]
