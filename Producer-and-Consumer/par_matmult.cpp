#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>

template <typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

template <Numeric T>
class MatrixRow;

template <Numeric T>
class MatrixColumn;

template <Numeric T>
class Matrix {
    size_t rows_;
    size_t cols_;
    int shmid_;
    T *data_;

  public:
    Matrix(size_t rows, size_t cols);
    ~Matrix();
    int shmid() const { return shmid_; }
    size_t rows() const { return rows_; }
    size_t cols() const { return cols_; }
    T *operator[](size_t idx) { return data_ + cols_ * idx; }
    const T *operator[](size_t idx) const { return data_ + cols_ * idx; }

    // Makes sense to call only after fork
    Matrix withNewAddr() const;

    MatrixRow<T> row(size_t row_idx) const {
        return MatrixRow<T>{*this, row_idx};
    }
    MatrixColumn<T> column(size_t col_idx) const {
        return MatrixColumn<T>{*this, col_idx};
    }

    Matrix operator*(const Matrix &right) const;

    template <Numeric U>
    friend std::ostream &operator<<(std::ostream &os, const Matrix<U> &mat);

  private:
    Matrix(size_t rows, size_t cols, int shmid, T *data)
        : rows_{rows}, cols_{cols}, shmid_{shmid}, data_{data} {}
};

template <Numeric T>
std::ostream &operator<<(std::ostream &os, const Matrix<T> &mat) {
    for (size_t i = 0; i < mat.rows(); i++) {
        for (size_t j = 0; j < mat.cols(); j++) {
            os << mat[i][j] << " ";
        }
        os << "\n";
    }
    return os;
}

template <Numeric T>
Matrix<T>::Matrix(size_t rows, size_t cols) : rows_{rows}, cols_{cols} {
    auto size{rows * cols * sizeof(T)};
    auto perm{0600};
    if ((shmid_ = shmget(IPC_PRIVATE, size, IPC_CREAT | IPC_EXCL | perm)) < 0) {
        perror("Failed to create shared memory");
        exit(1);
    }

    void *ptr;
    if ((ptr = shmat(shmid_, nullptr, 0)) == reinterpret_cast<void *>(-1)) {
        perror("Failed to attach to shared memory");
        exit(1);
    }
    data_ = static_cast<T *>(ptr);
}

template <Numeric T>
Matrix<T>::~Matrix() {
    if (shmdt(data_) != 0) {
        perror("Failed to detach shared memory");
    }
}

template <Numeric T>
Matrix<T> Matrix<T>::withNewAddr() const {
    void *ptr;
    if ((ptr = shmat(shmid_, nullptr, 0)) == reinterpret_cast<void *>(-1)) {
        perror("Failed to attach to shared memory");
        exit(1);
    }

    T *new_data{static_cast<T *>(ptr)};
    return Matrix(rows_, cols_, shmid_, new_data);
}

template <Numeric T>
class MatrixRow {
    const Matrix<T> *mat_;
    size_t row_idx_;

  public:
    MatrixRow(const Matrix<T> &mat, size_t row_idx)
        : mat_{&mat}, row_idx_{row_idx} {}

    T operator[](size_t col_idx) const { return (*mat_)[row_idx_][col_idx]; }
    size_t size() const { return mat_->cols(); }
};

template <Numeric T>
class MatrixColumn {
    const Matrix<T> *mat_;
    size_t col_idx_;

  public:
    MatrixColumn(const Matrix<T> &mat, size_t col_idx)
        : mat_{&mat}, col_idx_{col_idx} {}

    T operator[](size_t row_idx) const { return (*mat_)[row_idx][col_idx_]; }
    size_t size() const { return mat_->rows(); }
};

template <Numeric T>
struct ProcessData {
    MatrixRow<T> row;
    MatrixColumn<T> col;
    T *out;
};

template <Numeric T>
void mult(const ProcessData<T> &job) {
    assert(job.row.size() == job.col.size());
    T sum{0};
    for (size_t i{0}; i < job.row.size(); i++) sum += job.row[i] * job.col[i];
    *job.out = sum;
}

template <Numeric T>
Matrix<T> Matrix<T>::operator*(const Matrix<T> &right) const {
    if (cols() != right.rows()) {
        std::cerr << "Cannot multiply matrices with incompatible dimensions ("
                  << rows() << " x " << cols() << " with " << right.rows()
                  << " x " << right.cols() << ")" << std::endl;
        exit(1);
    }
    Matrix<T> result{rows(), right.cols()};
    for (size_t i{0}; i < result.rows(); i++) {
        for (size_t j{0}; j < result.cols(); j++) {
            auto pid = fork();
            if (pid == -1) {
                perror("failed to fork");
                exit(1);
            }
            if (pid == 0) {
                // Child process!
                // First create "copies" of matrices
                auto child_left = this->withNewAddr(),
                     child_right = right.withNewAddr(),
                     child_result = result.withNewAddr();
                // Create job
                ProcessData<T> job{.row = child_left.row(i),
                                   .col = child_right.column(j),
                                   .out = &child_result[i][j]};
                // Do the multiply
                mult(job);
                // Now kill child
                exit(1);
            }
        }
    }
    // Wait for all children to finish
    for (;;) {
        auto ret = wait(nullptr);
        if (ret == -1) {
            if (errno == ECHILD) break;
            perror("wait failed");
            exit(1);
        }
    }
    return result;
}

template <Numeric T>
void populateMaxMatrix(Matrix<T> &mat) {
    for (size_t i = 0; i < mat.rows(); i++) {
        for (size_t j = 0; j < mat.cols(); j++) {
            mat[i][j] = std::max(i + 1, j + 1);
        }
    }
}

template <Numeric T>
void populateMinMatrix(Matrix<T> &mat) {
    for (size_t i = 0; i < mat.rows(); i++) {
        for (size_t j = 0; j < mat.cols(); j++) {
            mat[i][j] = std::min(i + 1, j + 1);
        }
    }
}

template <Numeric T>
Matrix<T> inputMatrix() {
    int r, c;
    std::cout << "Enter no. of rows:\n";
    std::cin >> r;
    if (!std::cin.good() || r <= 0) {
        std::cerr << "Bad input!\n";
        exit(1);
    }
    std::cout << "Enter no. of columns\n";
    std::cin >> c;
    if (!std::cin.good() || c <= 0) {
        std::cerr << "Bad input!\n";
        exit(1);
    }
    Matrix<double> matrix(r, c);
    for (int i = 0; i < r; i++) {
        for (int j = 0; j < c; j++) {
            std::cout << "Enter (" << i << ", " << j << "):\n";
            std::cin >> matrix[i][j];
            if (!std::cin.good()) {
                std::cerr << "Bad input!\n";
                exit(1);
            }
        }
    }
    return matrix;
}

int main() {
    // Matrix<double> left(7, 7);
    // Matrix<double> right(7, 7);

    // populateMaxMatrix(left);
    // populateMinMatrix(right);

    // auto result = left * right;
    // std::cout << "Left: \n" << left;
    // std::cout << "\nRight: \n" << right;
    // std::cout << "\nResult: \n" << result;

    std::cout << "Enter left matrix:\n";
    auto left = inputMatrix<double>();
    std::cout << "Enter right matrix:\n";
    auto right = inputMatrix<double>();
    auto result = left * right;
    std::cout << "Left: \n" << left;
    std::cout << "\nRight: \n" << right;
    std::cout << "\nResult: \n" << result;
    return 0;
}
