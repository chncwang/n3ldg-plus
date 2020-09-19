#ifndef N3LDG_PLUS_MATRX_NODE_H
#define N3LDG_PLUS_MATRX_NODE_H

#include "Node.h"
#include "Graph.h"

class MatrixNode : public Node {
public:
    explicit MatrixNode(const string &node_type) : Node(node_type + "-matrix") {}

    int getColumn() const {
        return column_;
    }

    int getRow() const {
        return getDim() / column_;
    }

protected:
    void setColumn(int column) {
        if (getDim() % column != 0) {
            cerr << boost::format("MatrixNode setColumn - dim:%1% column:%2%") % getDim() % column
                << endl;
            abort();
        }
        column_ = column;
    }

private:
    int column_;
};

class MatrixExecutor : public Executor {
public:
    int getRow() const {
        return static_cast<MatrixNode *>(batch.front())->getRow();
    }

    vector<int> getCols() const {
        vector<int> cols;
        cols.reserve(batch.size());
        for (Node *node : batch) {
            MatrixNode *matrix = static_cast<MatrixNode *>(node);
            cols.push_back(matrix->getColumn());
        }
        return cols;
    }
};

class MatrixConcatNode : public MatrixNode, public Poolable<MatrixConcatNode> {
public:
    virtual int getKey() const override {
        return getDim();
    }

    virtual void initNode(int dim) override {
        init(dim);
    }

    virtual void setNodeDim(int dim) override {
        setDim(dim);
    }

    MatrixConcatNode(): MatrixNode("concat") {}

    void forward(Graph &graph, const vector<Node *> &inputs) {
        int input_dim = inputs.front()->getDim();
        for (auto it = inputs.begin() + 1; it != inputs.end(); ++it) {
            if (input_dim != (*it)->getDim()) {
                cerr << "MatrixConcatNode - forward inconsistent input dims" << endl;
                abort();
            }
        }

        in_nodes = inputs;
        for (Node *in : inputs) {
            in->addParent(this);
        }
        setColumn(inputs.size());
        graph.addNode(this);
    }

    void compute() override {
        for (int i = 0; i < in_nodes.size(); ++i) {
            int offset = i * getRow();
            for (int j = 0; j < getRow(); ++j) {
                val().v[offset + j] = in_nodes.at(i)->getVal().v[j];
            }
        }
    }

    void backward() override {
        for (int i = 0; i < in_nodes.size(); ++i) {
            int offset = i * getRow();
            for (int j = 0; j < getRow(); ++j) {
                in_nodes.at(i)->loss()[j] += loss()[offset + j];
            }
        }
    }

    bool typeEqual(Node *other) override {
        MatrixConcatNode *concat = static_cast<MatrixConcatNode*>(other);
        return concat->getNodeType() == getNodeType() && concat->getRow() == getRow();
    }

    string typeSignature() const override {
        return "MatrixConcatNode-" + to_string(getRow());
    }

    Executor* generate() override;

    const vector<Node *> &getInputs() const {
        return in_nodes;
    }

private:
    vector<Node *> in_nodes;
};

#if USE_GPU
class MatrixConcatExecutor : public MatrixExecutor {
public:
    void forward() override {
        for (Node *node : batch) {
            MatrixConcatNode *concat = static_cast<MatrixConcatNode*>(node);
//            cout << "in count:" << concat->getColumn() << endl;
            in_counts.push_back(concat->getColumn());
        }
        max_in_count = *max_element(in_counts.begin(), in_counts.end());
//        cout << "max_in_count:%d" << max_in_count << endl;
        vector<dtype *> vals, in_vals;
        int node_i = -1;
        for (Node *node : batch) {
            ++node_i;
            MatrixConcatNode *concat = static_cast<MatrixConcatNode*>(node);
            vals.push_back(concat->getVal().value);
            for (int i = 0; i < max_in_count; ++i) {
                in_vals.push_back(i < in_counts.at(node_i) ?
                        concat->getInputs().at(i)->getVal().value : nullptr);
            }
        }
        n3ldg_cuda::MatrixConcatForward(in_vals, getCount(), getRow(), in_counts, vals);
#if TEST_CUDA
        testForward();
        cout << "MatrixConcat forward tested" << endl;
#endif
    }

    void backward() override {
        vector<dtype *> grads, in_grads;
        int node_i = -1;
        for (Node *node : batch) {
            ++node_i;
            MatrixConcatNode *concat = static_cast<MatrixConcatNode*>(node);
            grads.push_back(concat->getLoss().value);
            for (int i = 0; i < max_in_count; ++i) {
                in_grads.push_back(i < in_counts.at(node_i) ?
                        concat->getInputs().at(i)->getLoss().value : nullptr);
            }
        }
        n3ldg_cuda::MatrixConcatBackward(grads, getCount(), getRow(), in_counts, in_grads);
#if TEST_CUDA
        auto get_inputs = [&](Node &node) {
            vector<pair<Node*, string>> pairs;
            MatrixConcatNode &concat = static_cast<MatrixConcatNode&>(node);
            for (Node *input : concat.getInputs()) {
                pairs.push_back(make_pair(input, input->getNodeType()));
            }
            return pairs;
        };
        testBackward(get_inputs);
#endif
    }

private:
    vector<int> in_counts;
    int max_in_count;
};
#else
class MatrixConcatExecutor : public MatrixExecutor {
public:
    int calculateFLOPs() override {
        return 0;
    }

    int calculateActivations() override {
        return 0;
    }
};
#endif

Executor* MatrixConcatNode::generate() {
    return new MatrixConcatExecutor;
}

class MatrixAndVectorPointwiseMultiExecutor;

class MatrixAndVectorPointwiseMultiNode : public MatrixNode,
    public Poolable<MatrixAndVectorPointwiseMultiNode> {
public:
    MatrixAndVectorPointwiseMultiNode() : MatrixNode("MatrixAndVectorPointwiseMulti") {}

    void setNodeDim(int dim) override {
        setDim(dim);
    }

    void initNode(int dim) override {
        init(dim);
    }

    void forward(Graph &graph, MatrixNode &matrix, Node &vec) {
        if (matrix.getRow() != vec.getDim()) {
            cerr << boost::format("MatrixConcatNode forward - matrix row:%1% vec dim:%2%") %
                matrix.getRow() % vec.getDim() << endl;
            abort();
        }
        matrix.addParent(this);
        vec.addParent(this);
        matrix_ = &matrix;
        vector_ = &vec;
        setColumn(matrix.getColumn());
        graph.addNode(this);
    }

    int getKey() const override {
        return getDim();
    }

    void compute() override {
        int in_dim = vector_->getDim();
        for (int i = 0; i < matrix_->getColumn(); ++i) {
            int matrix_i = i * in_dim;
            Vec(val().v + matrix_i, in_dim) = vector_->getVal().vec() *
                Vec(matrix_->getVal().v + matrix_i, in_dim);
        }
    }

    void backward() override {
        int in_dim = vector_->getDim();
        for (int i = 0; i < matrix_->getColumn(); ++i) {
            int matrix_i = i * in_dim;
            Vec(matrix_->loss().v + matrix_i, in_dim) += Vec(getLoss().v + matrix_i, in_dim) *
                vector_->getVal().vec();
            vector_->loss().vec() += Vec(getLoss().v + matrix_i, in_dim) *
                Vec(matrix_->getVal().v + matrix_i, in_dim);
        }
    }


    string typeSignature() const override {
        return "MatrixAndVectorPointwiseMulti-" + to_string(vector_->getDim());
    }

    bool typeEqual(Node *other) override {
        return getNodeType() == other->getNodeType() && vector_->getDim() ==
            static_cast<MatrixAndVectorPointwiseMultiNode *>(other)->vector_->getDim();
    }

    Executor *generate() override;

private:
    MatrixNode *matrix_ = nullptr;
    Node *vector_ = nullptr;
    friend class MatrixAndVectorPointwiseMultiExecutor;
};

#if USE_GPU
class MatrixAndVectorPointwiseMultiExecutor : public MatrixExecutor {
public:
    void forward() override {
        vector<dtype *> vals = getVals();
        for (Node *node : batch) {
            MatrixAndVectorPointwiseMultiNode *multi =
                static_cast<MatrixAndVectorPointwiseMultiNode *>(node);
            matrix_vals.push_back(multi->matrix_->getVal().value);
            vector_vals.push_back(multi->vector_->getVal().value);
        }
        int row = getRow();
        cols = getCols();
        n3ldg_cuda::MatrixAndVectorPointwiseMultiForward(matrix_vals, vector_vals, batch.size(),
                row, cols, vals);
#if TEST_CUDA
        testForward();
        cout << "MatrixAndVectorPointwiseMultiExecutor forward tested" << endl;
#endif
    }

    void backward() override {
        vector<dtype *> grads = getGrads();
        vector<dtype *> matrix_grads, vector_grads;
        for (Node *node : batch) {
            MatrixAndVectorPointwiseMultiNode *multi =
                static_cast<MatrixAndVectorPointwiseMultiNode *>(node);
            matrix_grads.push_back(multi->matrix_->getLoss().value);
            cout << "matrix grad dim:" << multi->matrix_->getDim() << endl;
            cout << "matrix grad tensor dim:" << multi->matrix_->getLoss().dim << endl;
            vector_grads.push_back(multi->vector_->getLoss().value);
        }
        n3ldg_cuda::MatrixAndVectorPointwiseMultiBackward(grads, matrix_vals, vector_vals,
                    batch.size(), getRow(), cols, matrix_grads, vector_grads);
#if TEST_CUDA
        auto get_inputs = [](Node &node)->vector<pair<Node *, string>> {
            MatrixAndVectorPointwiseMultiNode &multi =
                static_cast<MatrixAndVectorPointwiseMultiNode&>(node);
            return {make_pair(multi.matrix_, "matrix"), make_pair(multi.vector_, "vector")};
        };
        for (Node *node : batch) {
            MatrixAndVectorPointwiseMultiNode *multi =
                static_cast<MatrixAndVectorPointwiseMultiNode *>(node);
            cout << boost::format("matrix:%1% vec:%2%") % multi->matrix_ % multi->vector_ << endl;
            cout << boost::format("matrix loss:%1% vec loss:%2%") % multi->matrix_->getLoss().value
                % multi->vector_->getLoss().value << endl;
        }
        testBackward(get_inputs);
        cout << "MatrixAndVectorPointwiseMultiExecutor backward tested" << endl;
#endif
    }
private:
    vector<dtype *> matrix_vals, vector_vals;
    vector<int> cols;
};
#else
class MatrixAndVectorPointwiseMultiExecutor : public MatrixExecutor {
public:
    int calculateFLOPs() override {
        cerr << "MatrixAndVectorPointwiseMultiExecutor - calculateFLOPs" << endl;
        abort();
        return 0;
    }

    int calculateActivations() override {
        cerr << "MatrixAndVectorPointwiseMultiExecutor - calculateActivations" << endl;
        abort();
        return 0;
    }
};
#endif

Executor *MatrixAndVectorPointwiseMultiNode::generate() {
    return new MatrixAndVectorPointwiseMultiExecutor;
}

namespace n3ldg_plus {

MatrixNode *concatToMatrix(Graph &graph, const vector<Node *> &inputs) {
    int input_dim = inputs.front()->getDim();
    MatrixConcatNode *node = MatrixConcatNode::newNode(inputs.size() * input_dim);
    node->forward(graph, inputs);
    return node;
}

MatrixNode *pointwiseMultiply(Graph &graph, MatrixNode &matrix, Node &vec) {
    MatrixAndVectorPointwiseMultiNode *node = MatrixAndVectorPointwiseMultiNode::newNode(
            matrix.getDim());
    node->forward(graph, matrix, vec);
    return node;
}

}
#endif
