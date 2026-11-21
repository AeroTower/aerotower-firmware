# language: pt
Funcionalidade: Demonstrar a torre sem o hardware
  Como integrante da equipe na apresentação
  Quero simular uma torre que se comporta como o ESP32
  Para mostrar o painel funcionando mesmo sem a torre física

  Cenário: Torre do kit básico só informa a bomba
    Dado uma torre simulada do kit "basico" conectada ao broker
    Quando passa um intervalo de publicação
    Então o painel recebe só o estado da bomba

  Cenário: O alerta de nível baixo aparece sozinho
    Dado uma torre simulada do kit "completo" conectada ao broker
    E o reservatório está em 36%
    Quando passam 3 intervalos com a bomba ligada
    Então o painel recebe o tanque com status "baixo"

  Cenário: O botão de desligar do app para a bomba
    Dado uma torre simulada do kit "pro" conectada ao broker
    Quando o app manda o comando "desligar"
    Então a torre informa a bomba desligada e o ciclo parado
