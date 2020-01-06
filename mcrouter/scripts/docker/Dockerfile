FROM            ubuntu:14.04

MAINTAINER      mcrouter <mcrouter@fb.com>

ENV             MCROUTER_DIR            /usr/local/mcrouter
#ENV             MCROUTER_REPO           https://github.com/facebook/mcrouter.git
ENV             MCROUTER_REPO           https://github.com/codyja/mcrouter.git
ENV             DEBIAN_FRONTEND         noninteractive

RUN             apt-get install -y software-properties-common && apt-get update && apt-get install -y git curl

RUN             curl -s -O https://repo.anaconda.com/miniconda/Miniconda2-latest-Linux-x86_64.sh && \
                bash Miniconda2-latest-Linux-x86_64.sh -b -p $HOME/miniconda && \ 
                $HOME/miniconda/bin/conda install -y -c conda-forge fmt doxygen
                

RUN             mkdir -p $MCROUTER_DIR/repo && \
                cd $MCROUTER_DIR/repo && git clone -b changing-docker-install $MCROUTER_REPO && \
                cd $MCROUTER_DIR/repo/mcrouter/mcrouter/scripts && \
                ./install_ubuntu_14.04.sh $MCROUTER_DIR && \
                ./clean_ubuntu_14.04.sh $MCROUTER_DIR && rm -rf $MCROUTER_DIR/repo && \
                ln -s $MCROUTER_DIR/install/bin/mcrouter /usr/local/bin/mcrouter

ENV             DEBIAN_FRONTEND newt
