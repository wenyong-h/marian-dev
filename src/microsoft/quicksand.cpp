#include "quicksand.h"
#include "marian.h"

#ifdef MKL_FOUND
#include "mkl.h"
#endif


#include "translator/scorers.h"
#include "translator/beam_search.h"
#include "data/shortlist.h"

namespace marian {

namespace quicksand {

template <class T>
void set(Ptr<Options> options, const std::string& key, const T& value) {
    options->set(key, value);
}

template void set(Ptr<Options> options, const std::string& key, const size_t&);
template void set(Ptr<Options> options, const std::string& key, const int&);
template void set(Ptr<Options> options, const std::string& key, const std::string&);
template void set(Ptr<Options> options, const std::string& key, const bool&);
template void set(Ptr<Options> options, const std::string& key, const std::vector<std::string>&);


Ptr<Options> newOptions() {
  return New<Options>();
}

class BeamSearchDecoder : public IBeamSearchDecoder {
private:
    Ptr<ExpressionGraph> graph_;
    std::vector<Ptr<Scorer>> scorers_;

public:
    BeamSearchDecoder(Ptr<Options> options, Word eos)
    : IBeamSearchDecoder(options, eos) {
        //createLoggers();

        graph_ = New<ExpressionGraph>(true, true);
        graph_->setDevice(DeviceId{0, DeviceType::cpu});
        graph_->reserveWorkspaceMB(500);

#ifdef MKL_FOUND
        mkl_set_num_threads(options->get<size_t>("mkl-threads", 1));
#endif

        options_->set("inference", true);
        options_->set("word-penalty", 0);
        options_->set("normalize", 0);
        options_->set("n-best", false);

        // No unk in QS
        options_->set("allow-unk", false);

        std::vector<std::string> models = options_->get<std::vector<std::string>>("model");

        for(auto& model : models) {
            Ptr<Options> modelOpts = New<Options>();
            YAML::Node config;
            Config::GetYamlFromNpz(config, "special:model.yml", model);
            modelOpts->merge(options_);
            modelOpts->merge(config);
            auto encdec = models::from_options(modelOpts, models::usage::translation);
            scorers_.push_back(New<ScorerWrapper>(encdec, "F" + std::to_string(scorers_.size()), 1, model));
        }

        for(auto scorer : scorers_) {
          scorer->init(graph_);
        }
    }

    QSNBestBatch decode(const QSBatch& qsBatch, size_t maxLength,
                        const std::unordered_set<Word>& shortlist) {

        if(shortlist.size() > 0) {
          auto shortListGen = New<data::FakeShortlistGenerator>(shortlist);
          for(auto scorer : scorers_)
            scorer->setShortlistGenerator(shortListGen);
        }

        size_t batchSize = qsBatch.size();
        auto subBatch = New<data::SubBatch>(batchSize, maxLength, nullptr);
        for(size_t i = 0; i < maxLength; ++i) {
          for(size_t j = 0; j < batchSize; ++j) {
            const auto& sent = qsBatch[j];
            if(i < sent.size()) {
              size_t idx = i * batchSize + j;
              subBatch->data()[idx] = sent[i];
              subBatch->mask()[idx] = 1;
            }
          }
        }
        std::vector<Ptr<data::SubBatch>> subBatches;
        subBatches.push_back(subBatch);
        std::vector<size_t> sentIds(batchSize, 0);

        auto batch = New<data::CorpusBatch>(subBatches);
        batch->setSentenceIds(sentIds);

        auto search = New<BeamSearch>(options_, scorers_, eos_);
        auto histories = search->search(graph_, batch);

        QSNBest nbest;
        for(const auto& history : histories) {
          Result bestTranslation = history->Top();
          nbest.push_back(std::make_tuple(std::get<0>(bestTranslation),
                                          std::get<2>(bestTranslation)));
        }

        QSNBestBatch qsNbestBatch;
        qsNbestBatch.push_back(nbest);
        return qsNbestBatch;
    }

};

Ptr<IBeamSearchDecoder> newDecoder(Ptr<Options> options, Word eos) {
    return New<BeamSearchDecoder>(options, eos);
}

}
}
