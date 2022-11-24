`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================


Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.
Other repositories are likely forks, and code found there is not supported.


Build & install >>>> Construção e Instalação
---------------

You can find more information in our documentation (`docs/install.rst
<https://mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ mkdir build
  $ cd build
  $ meson ..
  $ sudo ninja install


Support >>>> Suporte 
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_
..........................................................................................
Muitos desenvolvedores do Mesa dependem do IRC; se você não tiver certeza de qual canal é
apropriado, você deve fazer sua pergunta no #dri-devel do OFTC
<irc://irc.oftc.net/dri-devel>`_, alguém irá redirecioná-lo se
necessário.
Lembre-se de que nem todos estão no mesmo fuso horário que você, então pode
demore um pouco até que alguém qualificado veja sua pergunta.
Para descobrir com quem você está falando, ou qual nick para o ping para o seu
pergunta, confira `Quem é quem no IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

A próxima melhor opção é fazer sua pergunta em um e-mail para o
listas de discussão: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_

Bug reports >>> relatos de bugs 
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://mesa3d.org/bugs.html>`_).
..........................................................................................
Se você acha que algo não está funcionando corretamente, envie um relatório de bug
(`docs/bugs.rst <https://mesa3d.org/bugs.html>`_).


Contributing >>> contribuição
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
..........................................................................................
Contribuições são bem-vindas e instruções passo a passo podem ser encontradas em nosso
documentação (`docs/submitpatches.rst
<https://mesa3d.org/submitpatches.html>`_).

Observe que o Mesa usa o gitlab para envio, revisão e discussões de patches.
