class Bm {
  constructor() {
  }

  async execute(from, inputString, parsedFilters, id) {
    // const Data = await userDatas.get(from);
    return {id: id, error: `0`, message :`Résultat de la commande`, output: null, filters: parsedFilters, data: []};
  }
}

module.exports = Bm;